/*
 * title-editor.cpp
 *
 * After Effects-style title editor.
 * Implemented with plain Qt widgets for maximum OBS compatibility.
 */

#include "title-editor.h"
#include "title-data.h"
#include "title-source.h"
#include "title-assets.h"
#include "title-localization.h"
#include "plugin-main.h"

#include <obs-module.h>

#include <QApplication>

#include <QBuffer>
#include <QIODevice>
#include <QPainter>
#include <QBrush>
#include <QPainterPath>
#include <QPolygonF>
#include <QLineF>
#include <QCursor>
#include <QImage>
#include <QImageReader>
#include <QSize>
#include <QSvgRenderer>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QResizeEvent>
#include <QCloseEvent>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSplitter>
#include <QToolBar>
#include <QAction>
#include <QActionGroup>
#include <QIcon>
#include <QPixmap>
#include <QStringList>
#include <QLocale>
#include <QStyle>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QSlider>
#include <QComboBox>
#include <QCheckBox>
#include <QGroupBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QColorDialog>
#include <QFileDialog>
#include <QInputDialog>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QRegularExpression>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QScrollArea>
#include <QScrollBar>
#include <QSizePolicy>
#include <QFrame>
#include <QSignalBlocker>
#include <QKeyEvent>
#include <QEvent>
#include <QKeySequence>
#include <QAbstractSpinBox>
#include <QAbstractItemModel>
#include <QTextEdit>
#include <QTextLayout>
#include <QTextOption>
#include <QDateTime>
#include <QTransform>
#include <QLinearGradient>
#include <QRadialGradient>
#include <QToolButton>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QContextMenuEvent>
#include <cmath>
#include <algorithm>
#include <vector>
#include <initializer_list>
#include <set>
#include <map>
#include <limits>
#include <tuple>
#include <functional>

namespace {


class NumericDragLabel : public QLabel {
public:
    NumericDragLabel(const QString &text, QWidget *field, QWidget *parent = nullptr,
                     std::function<void()> drag_started = {},
                     std::function<void()> drag_finished = {})
        : QLabel(text, parent), spin_box_(find_spin_box(field)),
          drag_started_(std::move(drag_started)), drag_finished_(std::move(drag_finished))
    {
        if (!spin_box_) return;
        setToolTip(obsgs_tr("OBSTitles.DragNumericLabelTooltip"));
    }

    ~NumericDragLabel() override
    {
        if (dragging_)
            QApplication::restoreOverrideCursor();
    }

protected:
    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton || !can_drag()) {
            QLabel::mousePressEvent(event);
            return;
        }

        dragging_ = true;
        drag_start_x_ = event->globalPosition().x();
        drag_start_value_ = spin_value();
        grabMouse(Qt::SizeHorCursor);
        QApplication::setOverrideCursor(Qt::SizeHorCursor);
        if (drag_started_)
            drag_started_();
        event->accept();
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (!dragging_) {
            QLabel::mouseMoveEvent(event);
            return;
        }
        if (!can_drag()) {
            finish_drag();
            event->accept();
            return;
        }

        const double delta = event->globalPosition().x() - drag_start_x_;
        set_spin_value(drag_start_value_ + delta * spin_step());
        event->accept();
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (!dragging_ || event->button() != Qt::LeftButton) {
            QLabel::mouseReleaseEvent(event);
            return;
        }

        finish_drag();
        event->accept();
    }

    void leaveEvent(QEvent *event) override
    {
        if (!dragging_)
            QLabel::leaveEvent(event);
    }

private:
    static QAbstractSpinBox *find_spin_box(QWidget *field)
    {
        if (!field) return nullptr;
        if (auto *spin = qobject_cast<QAbstractSpinBox *>(field)) return spin;
        return field->findChild<QAbstractSpinBox *>();
    }

    bool can_drag() const
    {
        return spin_box_ && spin_box_->isEnabled() && spin_box_->isVisible() && isEnabled();
    }

    double spin_value() const
    {
        if (auto *spin = qobject_cast<QDoubleSpinBox *>(spin_box_)) return spin->value();
        if (auto *spin = qobject_cast<QSpinBox *>(spin_box_)) return spin->value();
        return 0.0;
    }

    double spin_step() const
    {
        if (auto *spin = qobject_cast<QDoubleSpinBox *>(spin_box_)) return spin->singleStep();
        if (auto *spin = qobject_cast<QSpinBox *>(spin_box_)) return spin->singleStep();
        return 1.0;
    }

    void set_spin_value(double value)
    {
        if (auto *spin = qobject_cast<QDoubleSpinBox *>(spin_box_)) {
            spin->setValue(std::clamp(value, spin->minimum(), spin->maximum()));
        } else if (auto *spin = qobject_cast<QSpinBox *>(spin_box_)) {
            spin->setValue(std::clamp((int)std::round(value), spin->minimum(), spin->maximum()));
        }
    }

    void finish_drag()
    {
        dragging_ = false;
        releaseMouse();
        QApplication::restoreOverrideCursor();
        if (drag_finished_)
            drag_finished_();
    }

    QAbstractSpinBox *spin_box_ = nullptr;
    std::function<void()> drag_started_;
    std::function<void()> drag_finished_;
    bool dragging_ = false;
    double drag_start_x_ = 0.0;
    double drag_start_value_ = 0.0;
};

static double title_manual_screenshot_time(const Title &title)
{
    if (title.playback_mode == 1)
        return std::clamp(title.loop_start, 0.0, title.duration);
    if (title.playback_mode == 2)
        return std::clamp(title.pause_time, 0.0, title.duration);
    return std::clamp(title.duration * 0.5, 0.0, title.duration);
}

static std::string title_manual_screenshot_png_base64(const Title &title)
{
    const QImage screenshot = render_title_to_image(title, title_manual_screenshot_time(title));
    if (screenshot.isNull())
        return {};

    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    if (!screenshot.save(&buffer, "PNG"))
        return {};

    const QByteArray encoded = png.toBase64();
    return std::string(encoded.constData(), (size_t)encoded.size());
}

} // namespace

/* ────────────────────────────────────────────────────────────────── */
/*  Dark AE-style palette constants                                   */
/* ────────────────────────────────────────────────────────────────── */
static const QColor C_BG_DARK  { 0x1a1a1a };
static const QColor C_BG_MID   { 0x252525 };
static const QColor C_BG_LIGHT { 0x2e2e2e };
static const QColor C_ACCENT   { 0x0078d4 };

static bool editor_focus_accepts_text(QWidget *widget);

/* OBS safe area margins: Rec. ITU-R BT.1848-1 / EBU R 95. */
static constexpr double OBS_ACTION_SAFE_PERCENT = 0.035;
static constexpr double OBS_GRAPHICS_SAFE_PERCENT = 0.05;

/* Canvas editing controls are measured in view pixels so object scale never
 * changes their apparent size or proportions. */
static constexpr double CANVAS_CONTROL_SIZE_PX = 8.0;
static constexpr double CANVAS_CONTROL_HIT_RADIUS_PX = 8.0;
static constexpr double CANVAS_ROTATE_HIT_RADIUS_PX = 24.0;
static constexpr double CANVAS_ORIGIN_RADIUS_PX = 3.6;
static constexpr double CANVAS_ROTATION_SNAP_DEGREES = 15.0;

static bool editor_image_path_is_svg(const QString &path)
{
    return path.endsWith(QStringLiteral(".svg"), Qt::CaseInsensitive) ||
           path.endsWith(QStringLiteral(".svgz"), Qt::CaseInsensitive);
}

static double radians_to_degrees(double radians)
{
    return radians * 180.0 / 3.14159265358979323846;
}

static double degrees_to_radians(double degrees)
{
    return degrees * 3.14159265358979323846 / 180.0;
}

static double normalize_degrees(double degrees)
{
    while (degrees > 180.0) degrees -= 360.0;
    while (degrees <= -180.0) degrees += 360.0;
    return degrees;
}

static QPointF rotate_point_around(const QPointF &point, const QPointF &pivot, double degrees)
{
    double radians = degrees_to_radians(degrees);
    double c = std::cos(radians);
    double ss = std::sin(radians);
    double dx = point.x() - pivot.x();
    double dy = point.y() - pivot.y();
    return QPointF(pivot.x() + dx * c - dy * ss,
                   pivot.y() + dx * ss + dy * c);
}

static QCursor canvas_rotation_cursor()
{
    static const QCursor cursor = []() {
        QPixmap pixmap(24, 24);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(QPen(QColor(255, 255, 255), 2.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.drawArc(QRectF(5.0, 4.0, 14.0, 14.0), 35 * 16, 285 * 16);
        QPolygonF arrow;
        arrow << QPointF(17.0, 4.0) << QPointF(22.0, 5.5) << QPointF(18.8, 10.0);
        painter.setBrush(QColor(255, 255, 255));
        painter.drawPolygon(arrow);
        painter.setPen(QPen(QColor(0, 0, 0, 170), 1.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.setBrush(Qt::NoBrush);
        painter.drawArc(QRectF(5.0, 4.0, 14.0, 14.0), 35 * 16, 285 * 16);
        return QCursor(pixmap, 12, 12);
    }();
    return cursor;
}

static QSize editor_image_intrinsic_size(const QString &path)
{
    if (editor_image_path_is_svg(path)) {
        QSvgRenderer renderer(path);
        if (!renderer.isValid()) return QSize();
        QSize size = renderer.defaultSize();
        if (!size.isValid() || size.isEmpty())
            size = renderer.viewBox().size();
        return size;
    }

    QImageReader reader(path);
    reader.setAutoTransform(true);
    QSize size = reader.size();
    if (size.isValid() && !size.isEmpty())
        return size;

    QImage image = reader.read();
    return image.isNull() ? QSize() : image.size();
}

static QImage editor_load_layer_image(const QString &path, const QSize &fallback_size = QSize())
{
    if (editor_image_path_is_svg(path)) {
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

static const QColor C_TEXT     { 0xcccccc };
static const QColor C_RULER    { 0x1e1e1e };
static const QColor C_KF_DOT   { 0xffd23f };
static const QColor C_PLAYHEAD { 0xff4444 };

static QIcon keyframe_diamond_icon(bool active, bool outlined = false)
{
    if (active)
        return obsgs_icon("keyframe-active.svg");
    if (!outlined)
        return obsgs_icon("keyframe-inactive.svg");

    QPixmap pixmap(24, 24);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(QColor(0xff, 0xd2, 0x3f), 2));
    QPolygon diamond;
    diamond << QPoint(12, 5) << QPoint(19, 12) << QPoint(12, 19) << QPoint(5, 12);
    painter.drawPolygon(diamond);
    return QIcon(pixmap);
}





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

static QColor layer_type_color(LayerType type)
{
    switch (type) {
    case LayerType::Text:
        return QColor(0xb4, 0x5a, 0xa0);
    case LayerType::Clock:
        return QColor(0x4b, 0x9a, 0xc8);
    case LayerType::Ticker:
        return QColor(0xd8, 0x8a, 0x30);
    case LayerType::SolidRect:
    case LayerType::Shape:
        return QColor(0x4f, 0x8f, 0x58);
    case LayerType::Image:
        return QColor(0x7d, 0x8b, 0x7f);
    }
    return QColor(0x65, 0x8a, 0xc8);
}

static QColor layer_color(const Layer &layer, int /*row*/)
{
    return layer_type_color(layer.type);
}

static QString layer_type_short(LayerType type)
{
    switch (type) {
    case LayerType::Text: return "T";
    case LayerType::Clock: return "⏱";
    case LayerType::Ticker: return "↔";
    case LayerType::SolidRect: return "■";
    case LayerType::Image: return "▧";
    case LayerType::Shape: return "◆";
    }
    return "•";
}

static QIcon obs_icon(const char *file_name)
{
    return obsgs_icon(file_name);
}

static std::string editor_text_std(const char *key)
{
    return obsgs_tr(key).toStdString();
}

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
    font.setKerning(layer.font_kerning);
    font.setLetterSpacing(QFont::AbsoluteSpacing, layer.char_tracking);
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

static bool title_has_dynamic_text_layer(const std::shared_ptr<Title> &title)
{
    if (!title) return false;
    return std::any_of(title->layers.begin(), title->layers.end(), [](const std::shared_ptr<Layer> &layer) {
        return layer && (layer->type == LayerType::Clock || layer->type == LayerType::Ticker);
    });
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


static bool is_text_box_auto_size_layer(const Layer &layer)
{
    return layer.type == LayerType::Text || layer.type == LayerType::Clock;
}

static bool is_canvas_text_layer(const Layer &layer)
{
    return layer.type == LayerType::Text || layer.type == LayerType::Clock || layer.type == LayerType::Ticker;
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
    QColor color = color_from_argb(argb);
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
    const double angle = layer.gradient_angle * std::acos(-1.0) / 180.0;
    const double dx = std::cos(angle) * length;
    const double dy = std::sin(angle) * length;
    QLinearGradient gradient(QPointF(cx - dx, cy - dy), QPointF(cx + dx, cy + dy));
    gradient.setColorAt(start_pos, gradient_color_with_opacity(layer.gradient_start_color, opacity, layer.gradient_start_opacity));
    gradient.setColorAt(end_pos, gradient_color_with_opacity(layer.gradient_end_color, opacity, layer.gradient_end_opacity));
    return QBrush(gradient);
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
    double radians = eval_shadow_angle(layer, t) * 3.14159265358979323846 / 180.0;
    double distance = eval_shadow_distance(layer, t);
    return QPointF(std::cos(radians) * distance, std::sin(radians) * distance);
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

static void apply_easing_preset(Keyframe &keyframe, EasingType easing)
{
    keyframe.easing = easing;
    switch (easing) {
    case EasingType::Bezier:
        keyframe.cx1 = 0.33f; keyframe.cy1 = 0.0f;
        keyframe.cx2 = 0.67f; keyframe.cy2 = 1.0f;
        break;
    case EasingType::EaseIn:
        keyframe.cx1 = 0.42f; keyframe.cy1 = 0.0f;
        keyframe.cx2 = 1.0f; keyframe.cy2 = 1.0f;
        break;
    case EasingType::EaseOut:
        keyframe.cx1 = 0.0f; keyframe.cy1 = 0.0f;
        keyframe.cx2 = 0.58f; keyframe.cy2 = 1.0f;
        break;
    case EasingType::EaseInOut:
        keyframe.cx1 = 0.42f; keyframe.cy1 = 0.0f;
        keyframe.cx2 = 0.58f; keyframe.cy2 = 1.0f;
        break;
    case EasingType::Linear:
    case EasingType::Hold:
    default:
        keyframe.cx1 = 0.333f; keyframe.cy1 = 0.0f;
        keyframe.cx2 = 0.667f; keyframe.cy2 = 1.0f;
        break;
    }
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
    apply_easing_preset(kf, EasingType::Linear);
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

static void set_background_color_channels_at(Layer &layer, double time, uint32_t argb)
{
    set_animated_value(layer.background_color_a, time, (argb >> 24) & 0xFF);
    set_animated_value(layer.background_color_r, time, (argb >> 16) & 0xFF);
    set_animated_value(layer.background_color_g, time, (argb >> 8) & 0xFF);
    set_animated_value(layer.background_color_b, time, argb & 0xFF);
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

static bool any_keyframes(std::initializer_list<const AnimatedProperty *> props)
{
    for (const auto *prop : props)
        if (prop && prop->is_animated()) return true;
    return false;
}

static QStringList font_styles_for_family(const QString &family)
{
    QFontDatabase fdb;
    QStringList styles = fdb.styles(family);
    if (styles.isEmpty()) {
        styles << QStringLiteral("Regular");
    } else {
        styles.removeDuplicates();
        styles.sort(Qt::CaseInsensitive);
        int regular = styles.indexOf(QStringLiteral("Regular"));
        if (regular > 0) {
            QString item = styles.takeAt(regular);
            styles.prepend(item);
        }
    }
    return styles;
}

static void populate_font_style_combo(QComboBox *combo, const QString &family, const QString &preferred)
{
    if (!combo) return;
    QSignalBlocker blocker(combo);
    combo->clear();
    QStringList styles = font_styles_for_family(family);
    for (const QString &style : styles)
        combo->addItem(style, style);
    int idx = preferred.isEmpty() ? -1 : combo->findText(preferred);
    if (idx < 0) idx = combo->findText(QStringLiteral("Regular"));
    combo->setCurrentIndex(idx >= 0 ? idx : 0);
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


static QColor keyframe_color(EasingType)
{
    return C_KF_DOT;
}

static QPainterPath keyframe_shape_path(EasingType easing, const QPointF &center, qreal radius)
{
    const qreal x = center.x();
    const qreal y = center.y();
    QPainterPath path;

    switch (easing) {
    case EasingType::Linear:
        path.moveTo(x, y - radius);
        path.lineTo(x + radius, y);
        path.lineTo(x, y + radius);
        path.lineTo(x - radius, y);
        path.closeSubpath();
        break;
    case EasingType::EaseIn:
        path.moveTo(x - radius, y);
        path.lineTo(x, y - radius);
        path.lineTo(x + radius, y - radius);
        path.lineTo(x + radius, y + radius);
        path.lineTo(x, y + radius);
        path.closeSubpath();
        break;
    case EasingType::EaseOut:
        path.moveTo(x + radius, y);
        path.lineTo(x, y - radius);
        path.lineTo(x - radius, y - radius);
        path.lineTo(x - radius, y + radius);
        path.lineTo(x, y + radius);
        path.closeSubpath();
        break;
    case EasingType::EaseInOut:
        path.moveTo(x - radius, y - radius);
        path.lineTo(x + radius, y - radius);
        path.lineTo(x - radius, y + radius);
        path.lineTo(x + radius, y + radius);
        path.closeSubpath();
        break;
    case EasingType::Bezier:
        path.addEllipse(center, radius, radius);
        break;
    case EasingType::Hold:
        path.addRect(QRectF(x - radius, y - radius, radius * 2.0, radius * 2.0));
        break;
    default:
        path.moveTo(x, y - radius);
        path.lineTo(x + radius, y);
        path.lineTo(x, y + radius);
        path.lineTo(x - radius, y);
        path.closeSubpath();
        break;
    }

    return path;
}

static void draw_keyframe_marker(QPainter &painter, const QPointF &center, EasingType easing,
                                 qreal radius, const QColor &fill,
                                 const QColor &stroke, qreal stroke_width)
{
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setBrush(fill);
    painter.setPen(QPen(stroke, stroke_width, Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin));
    painter.drawPath(keyframe_shape_path(easing, center, radius));

    if (easing == EasingType::Bezier) {
        painter.setPen(QPen(stroke, std::max<qreal>(1.0, stroke_width - 0.5)));
        painter.drawLine(QPointF(center.x() - radius - 3.0, center.y()),
                         QPointF(center.x() - radius, center.y()));
        painter.drawLine(QPointF(center.x() + radius, center.y()),
                         QPointF(center.x() + radius + 3.0, center.y()));
    }

    painter.restore();
}

static QString easing_label(EasingType easing)
{
    switch (easing) {
    case EasingType::Linear: return obsgs_tr("OBSTitles.Linear");
    case EasingType::EaseIn: return obsgs_tr("OBSTitles.EaseIn");
    case EasingType::EaseOut: return obsgs_tr("OBSTitles.EaseOut");
    case EasingType::EaseInOut: return obsgs_tr("OBSTitles.EasyEase");
    case EasingType::Bezier: return obsgs_tr("OBSTitles.CustomBezier");
    case EasingType::Hold: return obsgs_tr("OBSTitles.Hold");
    default: return obsgs_tr("OBSTitles.Linear");
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
            &layer.background_enabled_prop, &layer.background_opacity_prop,
            &layer.background_padding_x_prop, &layer.background_padding_y_prop,
            &layer.background_corner_radius_prop,
            &layer.background_color_a, &layer.background_color_r,
            &layer.background_color_g, &layer.background_color_b,
            &layer.shadow_enabled_prop, &layer.shadow_opacity_prop,
            &layer.shadow_distance_prop, &layer.shadow_angle_prop,
            &layer.shadow_blur_prop, &layer.shadow_spread_prop,
            &layer.shadow_color_a, &layer.shadow_color_r,
            &layer.shadow_color_g, &layer.shadow_color_b};
}

static QString property_label(const std::string &name)
{
    if (name == "pos_x" || name == "pos_y") return obsgs_tr("OBSTitles.Position");
    if (name == "scale_x" || name == "scale_y") return obsgs_tr("OBSTitles.Scale");
    if (name == "box_width" || name == "box_height") return obsgs_tr("OBSTitles.Size");
    if (name == "origin_x" || name == "origin_y") return obsgs_tr("OBSTitles.Origin");
    if (name == "text_color_a" || name == "text_color_r" ||
        name == "text_color_g" || name == "text_color_b") return obsgs_tr("OBSTitles.TextColor");
    if (name == "fill_color_a" || name == "fill_color_r" ||
        name == "fill_color_g" || name == "fill_color_b") return obsgs_tr("OBSTitles.FillColor");
    if (name == "shadow_color_a" || name == "shadow_color_r" ||
        name == "shadow_color_g" || name == "shadow_color_b") return obsgs_tr("OBSTitles.ShadowColor");
    if (name == "background_color_a" || name == "background_color_r" ||
        name == "background_color_g" || name == "background_color_b") return obsgs_tr("OBSTitles.BackgroundColor");
    if (name == "background_enabled") return obsgs_tr("OBSTitles.EnableColorBackground");
    if (name == "background_opacity") return obsgs_tr("OBSTitles.BackgroundOpacityLabel");
    if (name == "background_padding_x") return obsgs_tr("OBSTitles.BackgroundHorizontalPaddingLabel");
    if (name == "background_padding_y") return obsgs_tr("OBSTitles.BackgroundVerticalPaddingLabel");
    if (name == "background_corner_radius") return obsgs_tr("OBSTitles.BackgroundCornerLabel");
    if (name == "shadow_enabled") return obsgs_tr("OBSTitles.ShadowEnable");
    if (name == "shadow_opacity") return obsgs_tr("OBSTitles.ShadowOpacity");
    if (name == "shadow_distance") return obsgs_tr("OBSTitles.ShadowDistance");
    if (name == "shadow_angle") return obsgs_tr("OBSTitles.ShadowAngle");
    if (name == "shadow_blur") return obsgs_tr("OBSTitles.ShadowBlur");
    if (name == "shadow_spread") return obsgs_tr("OBSTitles.ShadowSpread");
    if (name == "rotation") return obsgs_tr("OBSTitles.Rotation");
    if (name == "opacity") return obsgs_tr("OBSTitles.Opacity");
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
    if (prop.name == "opacity" || prop.name == "shadow_opacity" || prop.name == "background_opacity") value *= 100.0;
    if (prop.name == "shadow_enabled" || prop.name == "background_enabled") return value >= 0.5 ? obsgs_tr("OBSTitles.On") : obsgs_tr("OBSTitles.Off");
    return QString::number(value, 'f', (prop.name == "opacity" || prop.name == "shadow_opacity" || prop.name == "background_opacity") ? 1 : 2);
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
    setWindowTitle(obsgs_tr("OBSTitles.EditorWindowTitle"));
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

    clock_timer_ = new QTimer(this);
    clock_timer_->setInterval(33);
    connect(clock_timer_, &QTimer::timeout, this, [this]() {
        if (canvas_) canvas_->update();
    });
    clock_timer_->start();

    qApp->installEventFilter(this);
}

void TitleEditor::build_ui()
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    auto *menu_bar = new QMenuBar(this);
    auto *file_menu = menu_bar->addMenu(obsgs_tr("OBSTitles.FileMenu"));
    QAction *new_action = file_menu->addAction(obsgs_tr("OBSTitles.New"));
    new_action->setShortcut(QKeySequence::New);
    connect(new_action, &QAction::triggered, this, &TitleEditor::new_title_contents);
    QAction *save_action = file_menu->addAction(obs_icon("save.svg"), obsgs_tr("OBSTitles.Save"));
    save_action->setShortcut(QKeySequence::Save);
    connect(save_action, &QAction::triggered, this, &TitleEditor::save_title);
    QAction *save_as_new_action = file_menu->addAction(obsgs_tr("OBSTitles.SaveAsNew"));
    connect(save_as_new_action, &QAction::triggered, this, &TitleEditor::save_title_as_new);
    QAction *save_library_action = file_menu->addAction(obsgs_tr("OBSTitles.SaveInLibrary"));
    connect(save_library_action, &QAction::triggered, this, [this]() { export_title_template(true); });
    QAction *export_action = file_menu->addAction(obs_icon("export.svg"), obsgs_tr("OBSTitles.Export"));
    connect(export_action, &QAction::triggered, this, [this]() { export_title_template(false); });
    file_menu->addSeparator();
    QAction *exit_action = file_menu->addAction(obs_icon("file-exit.svg"), obsgs_tr("OBSTitles.Exit"));
    connect(exit_action, &QAction::triggered, this, &TitleEditor::close);

    auto *edit_menu = menu_bar->addMenu(obsgs_tr("OBSTitles.EditMenu"));
    edit_menu->addAction(act_undo_ = new QAction(obs_icon("undo.svg"), obsgs_tr("OBSTitles.Undo"), this));
    act_undo_->setShortcut(QKeySequence::Undo);
    connect(act_undo_, &QAction::triggered, this, [this]() {
        if (undo_index_ > 0) restore_undo_snapshot(undo_index_ - 1);
    });
    edit_menu->addAction(act_redo_ = new QAction(obs_icon("redo.svg"), obsgs_tr("OBSTitles.Redo"), this));
    act_redo_->setShortcut(QKeySequence::Redo);
    connect(act_redo_, &QAction::triggered, this, [this]() {
        if (undo_index_ + 1 < (int)undo_stack_.size()) restore_undo_snapshot(undo_index_ + 1);
    });
    edit_menu->addSeparator();
    QAction *copy_action = edit_menu->addAction(obsgs_tr("OBSTitles.Copy"));
    copy_action->setShortcut(QKeySequence::Copy);
    connect(copy_action, &QAction::triggered, this, [this]() {
        if (editor_focus_accepts_text(focusWidget())) return;
        if (timeline_ && timeline_->has_selected_keyframes()) {
            timeline_->copy_keyframe_selection();
            return;
        }
        copy_selected_layer();
    });
    QAction *cut_action = edit_menu->addAction(obsgs_tr("OBSTitles.Cut"));
    cut_action->setShortcut(QKeySequence::Cut);
    connect(cut_action, &QAction::triggered, this, [this]() {
        if (editor_focus_accepts_text(focusWidget())) return;
        if (timeline_ && timeline_->has_selected_keyframes()) {
            timeline_->cut_keyframe_selection();
            return;
        }
        cut_selected_layer();
    });
    QAction *paste_action = edit_menu->addAction(obsgs_tr("OBSTitles.Paste"));
    paste_action->setShortcut(QKeySequence::Paste);
    connect(paste_action, &QAction::triggered, this, [this]() {
        if (editor_focus_accepts_text(focusWidget())) return;
        if (timeline_ && timeline_->has_keyframe_clipboard()) {
            timeline_->paste_keyframes_at_playhead();
            return;
        }
        paste_layer_from_clipboard();
    });
    QAction *delete_action = edit_menu->addAction(obsgs_tr("OBSTitles.Delete"));
    delete_action->setShortcut(QKeySequence::Delete);
    connect(delete_action, &QAction::triggered, this, [this]() {
        if (editor_focus_accepts_text(focusWidget())) return;
        if (timeline_ && timeline_->has_selected_keyframes()) {
            timeline_->delete_keyframe_selection();
            return;
        }
        delete_selected_layer();
    });

    auto *view_menu = menu_bar->addMenu(QStringLiteral("View"));
    QAction *snap_action = view_menu->addAction(QStringLiteral("Snap"));
    snap_action->setCheckable(true);
    snap_action->setChecked(true);
    snap_action->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Semicolon));
    snap_action->setToolTip(QStringLiteral("Globally enable or disable snapping without changing Snap To targets."));
    connect(snap_action, &QAction::toggled, this, [this](bool enabled) {
        if (canvas_) canvas_->set_snap_enabled(enabled);
    });

    auto *snap_to_menu = view_menu->addMenu(QStringLiteral("Snap To"));
    auto add_snap_to_action = [this, snap_to_menu](const QString &text, bool checked, auto setter) {
        QAction *action = snap_to_menu->addAction(text);
        action->setCheckable(true);
        action->setChecked(checked);
        connect(action, &QAction::toggled, this, [this, setter](bool enabled) {
            if (canvas_) (canvas_->*setter)(enabled);
        });
        return action;
    };
    add_snap_to_action(QStringLiteral("Guides"), true, &CanvasPreview::set_snap_to_guides);
    add_snap_to_action(QStringLiteral("Grid"), false, &CanvasPreview::set_snap_to_grid);
    add_snap_to_action(QStringLiteral("Object Edges"), true, &CanvasPreview::set_snap_to_object_edges);
    add_snap_to_action(QStringLiteral("Object Centers"), true, &CanvasPreview::set_snap_to_object_centers);
    add_snap_to_action(QStringLiteral("Canvas Bounds"), true, &CanvasPreview::set_snap_to_canvas_bounds);
    add_snap_to_action(QStringLiteral("Spacing / Alignment"), true, &CanvasPreview::set_snap_to_spacing);

    auto *help_menu = menu_bar->addMenu(obsgs_tr("OBSTitles.HelpMenu"));
    QAction *about_action = help_menu->addAction(obs_icon("about.svg"), obsgs_tr("OBSTitles.About"));
    connect(about_action, &QAction::triggered, this, &TitleEditor::show_about);
    root->setMenuBar(menu_bar);

    /* ── Toolbar ── */
    build_toolbar();
    root->addWidget(toolbar_);

    /* ── Title name label bar ── */
    auto *title_bar = new QWidget(this);
    title_bar->setStyleSheet("background:#1e1e1e;");
    auto *title_bar_layout = new QHBoxLayout(title_bar);
    title_bar_layout->setContentsMargins(0, 3, 0, 3);
    title_bar_layout->setSpacing(6);
    title_bar_layout->addStretch(1);
    dirty_indicator_ = new QLabel(title_bar);
    dirty_indicator_->setFixedSize(10, 10);
    dirty_indicator_->setStyleSheet("background:#e33;border-radius:5px;");
    dirty_indicator_->setToolTip(obsgs_tr("OBSTitles.UnsavedChangesIndicator"));
    dirty_indicator_->hide();
    title_bar_layout->addWidget(dirty_indicator_, 0, Qt::AlignVCenter);
    title_lbl_ = new QLabel("—", title_bar);
    title_lbl_->setAlignment(Qt::AlignCenter);
    QFont tf = title_lbl_->font();
    tf.setPointSize(tf.pointSize() + 1);
    tf.setBold(true);
    title_lbl_->setFont(tf);
    title_lbl_->setStyleSheet("color:#fff;");
    title_bar_layout->addWidget(title_lbl_, 0, Qt::AlignVCenter);
    title_bar_layout->addStretch(1);
    root->addWidget(title_bar);

    /* ── Upper split: Global Settings | Canvas | Properties ── */
    auto *upper_split = new QSplitter(Qt::Horizontal, this);

    auto *global_panel = new QWidget(upper_split);
    auto *global_layout = new QVBoxLayout(global_panel);
    global_layout->setContentsMargins(0, 0, 0, 0);
    global_layout->setSpacing(4);
    title_props_ = new TitlePropertiesPanel(global_panel);
    global_layout->addWidget(title_props_);
    global_layout->addStretch(1);
    global_panel->setMinimumWidth(240);
    global_panel->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    upper_split->addWidget(global_panel);

    auto *canvas_panel = new QWidget(upper_split);
    auto *canvas_layout = new QVBoxLayout(canvas_panel);
    canvas_layout->setContentsMargins(0, 0, 0, 0);
    canvas_layout->setSpacing(0);
    canvas_ = new CanvasPreview(canvas_panel);
    canvas_->setMinimumSize(300, 200);
    canvas_layout->addWidget(canvas_, 1);

    auto *canvas_zoom_bar = new QWidget(canvas_panel);
    canvas_zoom_bar->setFixedHeight(34);
    canvas_zoom_bar->setStyleSheet(
        "QWidget{background:#171717;border-top:1px solid #333;}"
        "QPushButton,QToolButton{color:#ddd;background:#2a2a2a;border:1px solid #3f3f3f;border-radius:3px;padding:3px 8px;}"
        "QPushButton:hover,QToolButton:hover{background:#343434;}"
        "QToolButton::menu-indicator{image:none;}"
        "QSpinBox{color:#ddd;background:#202020;border:1px solid #3f3f3f;border-radius:3px;padding:2px 6px;}"
        "QSpinBox::up-button,QSpinBox::down-button{width:0;border:none;}"
        "QSlider::groove:horizontal{height:4px;background:#303030;border-radius:2px;}"
        "QSlider::handle:horizontal{width:12px;margin:-5px 0;background:#bfc7d5;border-radius:6px;}"
        "QSlider::sub-page:horizontal{background:#0078d4;border-radius:2px;}");
    auto *canvas_zoom_layout = new QHBoxLayout(canvas_zoom_bar);
    canvas_zoom_layout->setContentsMargins(10, 0, 10, 0);
    canvas_zoom_layout->setSpacing(8);
    auto *canvas_zoom_out = new QPushButton(canvas_zoom_bar);
    canvas_zoom_out->setIcon(obs_icon("zoom-out.svg"));
    canvas_zoom_out->setFixedWidth(30);
    auto *canvas_zoom_slider = new QSlider(Qt::Horizontal, canvas_zoom_bar);
    canvas_zoom_slider->setRange(5, 1600);
    canvas_zoom_slider->setValue(canvas_->zoom_percent());
    canvas_zoom_slider->setMinimumWidth(220);
    canvas_zoom_slider->setMaximumWidth(360);
    auto *canvas_zoom_in = new QPushButton(canvas_zoom_bar);
    canvas_zoom_in->setIcon(obs_icon("zoom-in.svg"));
    canvas_zoom_in->setFixedWidth(30);
    auto *canvas_zoom_percent = new QSpinBox(canvas_zoom_bar);
    canvas_zoom_percent->setRange(5, 1600);
    canvas_zoom_percent->setSuffix("%");
    canvas_zoom_percent->setAlignment(Qt::AlignCenter);
    canvas_zoom_percent->setButtonSymbols(QAbstractSpinBox::NoButtons);
    canvas_zoom_percent->setFixedWidth(72);
    canvas_zoom_percent->setValue(canvas_->zoom_percent());
    auto *fit_canvas = new QToolButton(canvas_zoom_bar);
    fit_canvas->setText("Fit");
    fit_canvas->setPopupMode(QToolButton::InstantPopup);
    auto *fit_canvas_menu = new QMenu(fit_canvas);
    auto add_canvas_zoom_action = [fit_canvas_menu](const QString &text, int percent) {
        QAction *action = fit_canvas_menu->addAction(text);
        action->setData(percent);
        return action;
    };
    QAction *fit_action = fit_canvas_menu->addAction("Fit");
    fit_action->setData(-1);
    QAction *fit_100_action = fit_canvas_menu->addAction("Fit up to 100%");
    fit_100_action->setData(-2);
    add_canvas_zoom_action("50%", 50);
    add_canvas_zoom_action("100%", 100);
    add_canvas_zoom_action("200%", 200);
    add_canvas_zoom_action("400%", 400);
    add_canvas_zoom_action("800%", 800);
    add_canvas_zoom_action("1600%", 1600);
    fit_canvas->setMenu(fit_canvas_menu);
    auto *checkerboard = new QToolButton(canvas_zoom_bar);
    checkerboard->setText("Checkerboard: Medium");
    checkerboard->setPopupMode(QToolButton::InstantPopup);
    auto *checkerboard_menu = new QMenu(checkerboard);
    auto add_checkerboard_action = [checkerboard_menu](const QString &text, int pattern) {
        QAction *action = checkerboard_menu->addAction(text);
        action->setData(pattern);
        return action;
    };
    add_checkerboard_action("Light", 0);
    add_checkerboard_action("Medium", 1);
    add_checkerboard_action("Dark", 2);
    checkerboard->setMenu(checkerboard_menu);
    canvas_zoom_layout->addWidget(canvas_zoom_out);
    canvas_zoom_layout->addWidget(canvas_zoom_slider);
    canvas_zoom_layout->addWidget(canvas_zoom_in);
    canvas_zoom_layout->addWidget(canvas_zoom_percent);
    canvas_zoom_layout->addWidget(fit_canvas);
    canvas_zoom_layout->addWidget(checkerboard);
    auto *safe_guides = new QToolButton(canvas_zoom_bar);
    safe_guides->setDefaultAction(act_safe_guides_);
    safe_guides->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    canvas_zoom_layout->addWidget(safe_guides);
    canvas_zoom_layout->addStretch(1);
    canvas_layout->addWidget(canvas_zoom_bar);
    connect(canvas_zoom_slider, &QSlider::valueChanged, canvas_, &CanvasPreview::set_zoom_percent);
    connect(canvas_zoom_percent, qOverload<int>(&QSpinBox::valueChanged), canvas_, &CanvasPreview::set_zoom_percent);
    connect(canvas_, &CanvasPreview::zoom_percent_changed, this, [canvas_zoom_slider, canvas_zoom_percent](int percent) {
        QSignalBlocker slider_blocker(canvas_zoom_slider);
        QSignalBlocker spin_blocker(canvas_zoom_percent);
        canvas_zoom_slider->setValue(percent);
        canvas_zoom_percent->setValue(percent);
    });
    connect(canvas_zoom_out, &QPushButton::clicked, this, [this]() {
        canvas_->set_zoom_percent((int)std::round(canvas_->zoom_percent() / 1.18));
    });
    connect(canvas_zoom_in, &QPushButton::clicked, this, [this]() {
        canvas_->set_zoom_percent((int)std::round(canvas_->zoom_percent() * 1.18));
    });
    connect(fit_canvas_menu, &QMenu::triggered, this, [this, fit_canvas](QAction *action) {
        int value = action->data().toInt();
        fit_canvas->setText(action->text());
        if (value == -1) canvas_->fit_canvas(false);
        else if (value == -2) canvas_->fit_canvas(true);
        else canvas_->set_zoom_percent(value);
    });
    connect(checkerboard_menu, &QMenu::triggered, this, [this, checkerboard](QAction *action) {
        checkerboard->setText(QString("Checkerboard: %1").arg(action->text()));
        canvas_->set_checkerboard_pattern(action->data().toInt());
    });
    upper_split->addWidget(canvas_panel);

    auto *side_panel = new QWidget(upper_split);
    auto *side_layout = new QVBoxLayout(side_panel);
    side_layout->setContentsMargins(0, 0, 0, 0);
    side_layout->setSpacing(4);
    props_ = new PropertiesPanel(side_panel);
    side_layout->addWidget(props_, 1);
    side_panel->setMinimumWidth(260);
    side_panel->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    upper_split->addWidget(side_panel);
    upper_split->setStretchFactor(0, 0);
    upper_split->setStretchFactor(1, 3);
    upper_split->setStretchFactor(2, 1);

    /* ── Timeline editor: full-width transport | LayerStack + Timeline | full-width zoom ── */
    auto *timeline_editor = new QWidget(this);
    auto *timeline_editor_layout = new QVBoxLayout(timeline_editor);
    timeline_editor_layout->setContentsMargins(0, 0, 0, 0);
    timeline_editor_layout->setSpacing(0);

    auto *timeline_transport = new QWidget(timeline_editor);
    timeline_transport->setFixedHeight(34);
    timeline_transport->setStyleSheet(
        "QWidget{background:#141414;border-bottom:1px solid #333;}"
        "QToolButton{color:#ccc;background:transparent;padding:3px 7px;border:none;}"
        "QToolButton:hover{background:#333;border-radius:2px;}"
        "QLabel{color:#0af;font-family:monospace;}");
    auto *transport_layout = new QHBoxLayout(timeline_transport);
    transport_layout->setContentsMargins(8, 0, 8, 0);
    transport_layout->setSpacing(2);
    auto make_transport_button = [timeline_transport](QAction *action) {
        auto *button = new QToolButton(timeline_transport);
        button->setDefaultAction(action);
        button->setIconSize(QSize(14, 14));
        button->setAutoRaise(true);
        return button;
    };
    transport_layout->addStretch(1);
    transport_layout->addWidget(make_transport_button(act_rew_));
    transport_layout->addWidget(make_transport_button(act_prev_kf_));
    transport_layout->addWidget(make_transport_button(act_play_));
    transport_layout->addWidget(make_transport_button(act_full_loop_));
    QAction *step_forward_action = new QAction(obs_icon("step-forward.svg"), obsgs_tr("OBSTitles.StepForward"), timeline_transport);
    connect(step_forward_action, &QAction::triggered, this, &TitleEditor::step_forward);
    transport_layout->addWidget(make_transport_button(step_forward_action));
    transport_layout->addWidget(make_transport_button(act_next_kf_));
    transport_layout->addStretch(1);
    timeline_editor_layout->addWidget(timeline_transport);

    auto *lower_split = new QSplitter(Qt::Horizontal, timeline_editor);

    auto *layers_panel = new QWidget(lower_split);
    auto *layers_layout = new QVBoxLayout(layers_panel);
    layers_layout->setContentsMargins(0, 0, 0, 0);
    layers_layout->setSpacing(0);

    layers_ = new LayerStack(layers_panel);
    layers_->setMinimumHeight(140);
    layers_layout->addWidget(layers_, 1);
    layers_panel->setMinimumWidth(280);
    layers_panel->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    lower_split->addWidget(layers_panel);

    auto *timeline_panel = new QWidget(lower_split);
    auto *timeline_panel_layout = new QVBoxLayout(timeline_panel);
    timeline_panel_layout->setContentsMargins(0, 0, 0, 0);
    timeline_panel_layout->setSpacing(0);

    timeline_ = new TimelineWidget(timeline_panel);
    timeline_->setMinimumHeight(140);
    timeline_panel_layout->addWidget(timeline_, 1);

    auto *timeline_zoom_bar = new QWidget(timeline_panel);
    timeline_zoom_bar->setFixedHeight(34);
    timeline_zoom_bar->setStyleSheet(
        "QWidget{background:#171717;border-top:1px solid #333;}"
        "QPushButton{color:#ddd;background:#2a2a2a;border:1px solid #3f3f3f;border-radius:3px;padding:3px 8px;}"
        "QPushButton:hover{background:#343434;}"
        "QSlider::groove:horizontal{height:4px;background:#303030;border-radius:2px;}"
        "QSlider::handle:horizontal{width:12px;margin:-5px 0;background:#bfc7d5;border-radius:6px;}"
        "QSlider::sub-page:horizontal{background:#0078d4;border-radius:2px;}");
    auto *zoom_layout = new QHBoxLayout(timeline_zoom_bar);
    zoom_layout->setContentsMargins(10, 0, 10, 0);
    zoom_layout->setSpacing(8);
    auto *zoom_out = new QPushButton(timeline_zoom_bar);
    zoom_out->setIcon(obs_icon("zoom-out.svg"));
    zoom_out->setFixedWidth(30);
    auto *zoom_slider = new QSlider(Qt::Horizontal, timeline_zoom_bar);
    zoom_slider->setRange(5, 1200);
    zoom_slider->setValue(timeline_->zoom_percent());
    zoom_slider->setMinimumWidth(220);
    zoom_slider->setMaximumWidth(360);
    auto *zoom_in = new QPushButton(timeline_zoom_bar);
    zoom_in->setIcon(obs_icon("zoom-in.svg"));
    zoom_in->setFixedWidth(30);
    auto *fit_timeline = new QPushButton(obsgs_tr("OBSTitles.FitTimeline"), timeline_zoom_bar);
    zoom_layout->addWidget(zoom_out);
    zoom_layout->addWidget(zoom_slider);
    zoom_layout->addWidget(zoom_in);
    zoom_layout->addWidget(fit_timeline);
    zoom_layout->addStretch(1);
    connect(zoom_slider, &QSlider::valueChanged, timeline_, &TimelineWidget::set_zoom_percent);
    connect(timeline_, &TimelineWidget::zoom_percent_changed, this, [zoom_slider](int percent) {
        QSignalBlocker blocker(zoom_slider);
        zoom_slider->setValue(percent);
    });
    connect(zoom_out, &QPushButton::clicked, this, [this]() {
        timeline_->set_zoom_percent((int)std::round(timeline_->zoom_percent() / 1.18));
    });
    connect(zoom_in, &QPushButton::clicked, this, [this]() {
        timeline_->set_zoom_percent((int)std::round(timeline_->zoom_percent() * 1.18));
    });
    connect(fit_timeline, &QPushButton::clicked, timeline_, &TimelineWidget::fit_timeline);
    timeline_panel_layout->addWidget(timeline_zoom_bar);
    lower_split->addWidget(timeline_panel);

    if (auto *scroll_bar = layers_->vertical_scroll_bar()) {
        connect(scroll_bar, &QScrollBar::valueChanged, timeline_, &TimelineWidget::set_vertical_scroll);
        connect(timeline_, &TimelineWidget::vertical_scroll_delta_requested, this,
                [scroll_bar](int delta) { scroll_bar->setValue(scroll_bar->value() + delta); });
    }
    lower_split->setStretchFactor(0, 1);
    lower_split->setStretchFactor(1, 3);
    lower_split->setCollapsible(0, false);
    lower_split->setCollapsible(1, false);
    timeline_editor_layout->addWidget(lower_split, 1);

    /* ── Outer vertical split ── */
    auto *vsplit = new QSplitter(Qt::Vertical, this);
    vsplit->addWidget(upper_split);
    vsplit->addWidget(timeline_editor);
    vsplit->setStretchFactor(0, 3);
    vsplit->setStretchFactor(1, 2);
    root->addWidget(vsplit, 1);

    /* ── Connect sub-widget signals ── */
    connect(layers_, &LayerStack::layer_selected,
            this, &TitleEditor::on_layer_selected);
    connect(layers_, &LayerStack::layers_selected,
            this, [this](const std::vector<std::string> &ids) {
                sel_layer_id_ = ids.empty() ? std::string() : ids.back();
                canvas_->set_selected_layers(ids);
                timeline_->set_selected_layer(sel_layer_id_);
                if (!title_) return;
                auto layer = title_->find_layer(sel_layer_id_);
                if (layer) props_->set_layer(layer, playhead_);
            });

    connect(layers_, &LayerStack::add_layer_requested,
            this, [this](LayerType type) {
                if (!title_) return;
                auto l = std::make_shared<Layer>();
                l->id   = TitleDataStore::make_uuid();
                l->name = (type == LayerType::Text) ? editor_text_std("OBSTitles.Text") :
                          (type == LayerType::Clock) ? editor_text_std("OBSTitles.Clock") :
                          (type == LayerType::Ticker) ? editor_text_std("OBSTitles.Ticker") :
                          (type == LayerType::Image) ? editor_text_std("OBSTitles.Image") : editor_text_std("OBSTitles.Rectangle");
                l->type = type;
                l->text_content = (type == LayerType::Text) ? editor_text_std("OBSTitles.NewText") :
                                  (type == LayerType::Ticker) ? editor_text_std("OBSTitles.NewTickerText") : "";
                l->clock_format = (type == LayerType::Clock) ? "H:i:s" : l->clock_format;
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
                        this, obsgs_tr("OBSTitles.ChooseImage"), QString(),
                        obsgs_tr("OBSTitles.ImageFileFilter"));
                    if (path.isEmpty()) return;
                    l->image_path = path.toStdString();
                    QSize image_size = editor_image_intrinsic_size(path);
                    if (image_size.isValid() && !image_size.isEmpty()) {
                        l->rect_width = (float)image_size.width();
                        l->rect_height = (float)image_size.height();
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

    connect(layers_, &LayerStack::clone_layer_requested,
            this, [this](const std::string &lid) {
                if (!title_) return;
                auto original = title_->find_layer(lid);
                if (!original) return;
                auto clone = clone_layer_for_insert(*original, true);
                std::string clone_id = clone->id;
                insert_layer_above(lid, clone);
                select_after_layer_list_mutation(clone_id);
                on_title_modified();
            });

    connect(layers_, &LayerStack::copy_layer_requested,
            this, [this](const std::string &lid) {
                on_layer_selected(lid);
                copy_selected_layer();
            });

    connect(layers_, &LayerStack::paste_layer_requested,
            this, [this](const std::string &anchor_id) {
                if (!anchor_id.empty()) on_layer_selected(anchor_id);
                paste_layer_from_clipboard();
            });

    connect(layers_, &LayerStack::delete_layer_requested,
            this, [this](const std::string &lid) {
                on_layer_selected(lid);
                delete_selected_layer();
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
                    layer->name = name.empty() ? editor_text_std("OBSTitles.Layer") : name;
                    timeline_->set_title(title_);
                    on_title_modified();
                    QTimer::singleShot(0, layers_, [this]() {
                        if (layers_) layers_->refresh();
                    });
                }
            });

    connect(timeline_, &TimelineWidget::playhead_changed,
            this, &TitleEditor::on_playhead_changed);
    connect(timeline_, &TimelineWidget::layer_selected,
            this, &TitleEditor::on_layer_selected);
    connect(timeline_, &TimelineWidget::keyframe_easing_changed,
            this, [this]() { on_title_modified(); });

    connect(props_, &PropertiesPanel::property_changed,
            this, &TitleEditor::on_title_modified);
    connect(title_props_, &TitlePropertiesPanel::title_changed,
            this, [this](bool push_undo_snapshot) {
                if (!title_) return;
                playhead_ = std::clamp(playhead_, 0.0, title_->duration);
                on_title_modified(push_undo_snapshot);
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
    connect(canvas_, &CanvasPreview::layers_selected,
            this, [this](const std::vector<std::string> &ids) {
                sel_layer_id_ = ids.empty() ? std::string() : ids.back();
                layers_->set_selected_layers(ids);
                canvas_->set_selected_layers(ids);
                timeline_->set_selected_layer(sel_layer_id_);
                if (!title_) return;
                auto layer = title_->find_layer(sel_layer_id_);
                if (layer) props_->set_layer(layer, playhead_);
            });
    connect(canvas_, &CanvasPreview::layer_geometry_changed,
            this, [this]() {
                on_title_modified();
                if (title_ && !sel_layer_id_.empty()) {
                    if (auto layer = title_->find_layer(sel_layer_id_))
                        props_->set_layer(layer, playhead_);
                }
            });
    connect(canvas_, &CanvasPreview::layer_structure_changed,
            this, [this]() {
                layers_->refresh();
                timeline_->set_title(title_);
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


void TitleEditor::flip_selected_layers(bool horizontal)
{
    if (!title_ || sel_layer_id_.empty()) return;
    auto ids = layers_ ? layers_->selected_ids() : std::vector<std::string>{sel_layer_id_};
    if (ids.empty()) return;

    std::shared_ptr<Layer> last_layer;
    for (const auto &id : ids) {
        auto layer = title_->find_layer(id);
        if (!layer || layer->locked) continue;
        const double lt = std::clamp(playhead_ - layer->in_time, 0.0,
                                     std::max(0.0, layer->out_time - layer->in_time));
        AnimatedProperty &prop = horizontal ? layer->scale_x : layer->scale_y;
        const double current = prop.evaluate(lt);
        set_animated_value(prop, lt, -current);
        last_layer = layer;
    }

    if (!last_layer) return;
    on_title_modified();
    if (props_) props_->set_layer(last_layer, playhead_);
}

void TitleEditor::rotate_selected_layers(double degrees)
{
    if (!title_ || sel_layer_id_.empty()) return;
    auto ids = layers_ ? layers_->selected_ids() : std::vector<std::string>{sel_layer_id_};
    if (ids.empty()) return;

    std::shared_ptr<Layer> last_layer;
    for (const auto &id : ids) {
        auto layer = title_->find_layer(id);
        if (!layer || layer->locked) continue;
        const double lt = std::clamp(playhead_ - layer->in_time, 0.0,
                                     std::max(0.0, layer->out_time - layer->in_time));
        set_animated_value(layer->rotation, lt, layer->rotation.evaluate(lt) + degrees);
        last_layer = layer;
    }

    if (!last_layer) return;
    on_title_modified();
    if (props_) props_->set_layer(last_layer, playhead_);
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
        double x0 = layer->pos_x.evaluate(lt) - layer->origin_x * width * sx;
        double x1 = layer->pos_x.evaluate(lt) + (1.0 - layer->origin_x) * width * sx;
        double y0 = layer->pos_y.evaluate(lt) - layer->origin_y * height * sy;
        double y1 = layer->pos_y.evaluate(lt) + (1.0 - layer->origin_y) * height * sy;
        double left = std::min(x0, x1);
        double right = std::max(x0, x1);
        double top = std::min(y0, y1);
        double bottom = std::max(y0, y1);
        min_left = std::min(min_left, left);
        max_right = std::max(max_right, right);
        min_top = std::min(min_top, top);
        max_bottom = std::max(max_bottom, bottom);
        entries.push_back({layer, lt, width, height, sx, sy});
    }

    if (entries.empty()) return;
    if (alignment_target_ == 0 && entries.size() < 2) return;

    double target_left = min_left;
    double target_hcenter = (min_left + max_right) / 2.0;
    double target_right = max_right;
    double target_top = min_top;
    double target_vcenter = (min_top + max_bottom) / 2.0;
    double target_bottom = max_bottom;

    if (alignment_target_ == 1 || alignment_target_ == 2) {
        const double safe_inset = alignment_target_ == 1 ? OBS_GRAPHICS_SAFE_PERCENT : OBS_ACTION_SAFE_PERCENT;
        target_left = title_->width * safe_inset;
        target_hcenter = title_->width / 2.0;
        target_right = title_->width * (1.0 - safe_inset);
        target_top = title_->height * safe_inset;
        target_vcenter = title_->height / 2.0;
        target_bottom = title_->height * (1.0 - safe_inset);
    } else if (alignment_target_ == 3) {
        target_left = 0.0;
        target_hcenter = title_->width / 2.0;
        target_right = title_->width;
        target_top = 0.0;
        target_vcenter = title_->height / 2.0;
        target_bottom = title_->height;
    }

    std::shared_ptr<Layer> last_layer;
    for (const auto &entry : entries) {
        if (x_mode >= 0) {
            const double x0 = -entry.layer->origin_x * entry.width * entry.scale_x;
            const double x1 = (1.0 - entry.layer->origin_x) * entry.width * entry.scale_x;
            const double left_offset = std::min(x0, x1);
            const double right_offset = std::max(x0, x1);
            double next_x = entry.layer->pos_x.evaluate(entry.lt);
            if (x_mode == 0) next_x = target_left - left_offset;
            if (x_mode == 1) next_x = target_hcenter - (left_offset + right_offset) / 2.0;
            if (x_mode == 2) next_x = target_right - right_offset;
            set_animated_value(entry.layer->pos_x, entry.lt, next_x);
        }
        if (y_mode >= 0) {
            const double y0 = -entry.layer->origin_y * entry.height * entry.scale_y;
            const double y1 = (1.0 - entry.layer->origin_y) * entry.height * entry.scale_y;
            const double top_offset = std::min(y0, y1);
            const double bottom_offset = std::max(y0, y1);
            double next_y = entry.layer->pos_y.evaluate(entry.lt);
            if (y_mode == 0) next_y = target_top - top_offset;
            if (y_mode == 1) next_y = target_vcenter - (top_offset + bottom_offset) / 2.0;
            if (y_mode == 2) next_y = target_bottom - bottom_offset;
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
    toolbar_->setToolButtonStyle(Qt::ToolButtonIconOnly);
    toolbar_->setIconSize(QSize(16, 16));
    toolbar_->setStyleSheet(
        "QToolBar { background:#1a1a1a; border-bottom:1px solid #333; spacing:2px; }"
        "QToolButton { color:#ccc; background:transparent; padding:4px 8px; border:none; }"
        "QToolButton:hover { background:#333; border-radius:3px; }"
        "QToolButton:pressed { background:#0078d4; }"
        "QToolButton:checked { background:#0078d4; color:#fff; border-radius:3px; }");

    act_rew_ = new QAction(obs_icon("rewind.svg"), obsgs_tr("OBSTitles.Rewind"), this);
    act_prev_kf_ = new QAction(obs_icon("previous-keyframe.svg"), obsgs_tr("OBSTitles.PreviousKeyframe"), this);
    act_play_ = new QAction(obs_icon("play.svg"), obsgs_tr("OBSTitles.Play"), this);
    act_full_loop_ = new QAction(obs_icon("loop.svg"), obsgs_tr("OBSTitles.LoopPreview"), this);
    act_full_loop_->setToolTip(obsgs_tr("OBSTitles.LoopPreviewTooltip"));
    act_next_kf_ = new QAction(obs_icon("next-keyframe.svg"), obsgs_tr("OBSTitles.NextKeyframe"), this);

    connect(act_rew_, &QAction::triggered, this, &TitleEditor::rewind);
    connect(act_prev_kf_, &QAction::triggered, this, &TitleEditor::previous_keyframe);
    connect(act_play_, &QAction::triggered, this, &TitleEditor::play_pause);
    connect(act_full_loop_, &QAction::triggered, this, &TitleEditor::play_full_loop);
    connect(act_next_kf_, &QAction::triggered, this, &TitleEditor::next_keyframe);

    time_lbl_ = new QLabel("0.000 s", toolbar_);
    time_lbl_->setStyleSheet("color:#0af; font-family:monospace; min-width:70px;");
    toolbar_->addWidget(time_lbl_);

    toolbar_->addSeparator();
    auto *align_target = new QToolButton(toolbar_);
    align_target->setIcon(obs_icon("alignment-target.svg"));
    align_target->setToolButtonStyle(Qt::ToolButtonIconOnly);
    align_target->setToolTip(obsgs_tr("OBSTitles.AlignmentTarget"));
    align_target->setAccessibleName(obsgs_tr("OBSTitles.AlignmentTarget"));
    align_target->setPopupMode(QToolButton::InstantPopup);
    align_target->setStyleSheet("QToolButton{color:#ddd;background:#3a3a3a;border:1px solid #666;border-radius:2px;padding:3px 6px;} QToolButton::menu-indicator{image:none;}");
    auto *align_menu = new QMenu(align_target);
    QAction *target_selection = align_menu->addAction(obsgs_tr("OBSTitles.AlignToSelection"));
    QAction *target_title_safe = align_menu->addAction(obsgs_tr("OBSTitles.AlignToTitleSafeGuides"));
    QAction *target_action_safe = align_menu->addAction(obsgs_tr("OBSTitles.AlignToActionSafeGuides"));
    QAction *target_artboard = align_menu->addAction(obsgs_tr("OBSTitles.AlignToArtboard"));
    target_selection->setCheckable(true);
    target_title_safe->setCheckable(true);
    target_action_safe->setCheckable(true);
    target_artboard->setCheckable(true);
    target_artboard->setChecked(true);
    auto update_alignment_target = [this, align_target, target_selection, target_title_safe, target_action_safe, target_artboard](int target) {
        alignment_target_ = target;
        target_selection->setChecked(target == 0);
        target_title_safe->setChecked(target == 1);
        target_action_safe->setChecked(target == 2);
        target_artboard->setChecked(target == 3);
        QString tooltip = obsgs_tr("OBSTitles.AlignToArtboard");
        if (target == 0)
            tooltip = obsgs_tr("OBSTitles.AlignToSelection");
        else if (target == 1)
            tooltip = obsgs_tr("OBSTitles.AlignToTitleSafeGuides");
        else if (target == 2)
            tooltip = obsgs_tr("OBSTitles.AlignToActionSafeGuides");
        align_target->setToolTip(tooltip);
    };
    connect(target_selection, &QAction::triggered, this, [update_alignment_target]() { update_alignment_target(0); });
    connect(target_title_safe, &QAction::triggered, this, [update_alignment_target]() { update_alignment_target(1); });
    connect(target_action_safe, &QAction::triggered, this, [update_alignment_target]() { update_alignment_target(2); });
    connect(target_artboard, &QAction::triggered, this, [update_alignment_target]() { update_alignment_target(3); });
    align_target->setMenu(align_menu);
    toolbar_->addWidget(align_target);

    auto add_align_action = [this](const char *icon_name, const QString &tip, int x_mode, int y_mode) {
        QAction *action = toolbar_->addAction(obs_icon(icon_name), tip);
        action->setToolTip(tip);
        connect(action, &QAction::triggered, this, [this, x_mode, y_mode]() {
            align_selected_layers(x_mode, y_mode);
        });
        return action;
    };
    add_align_action("align-left.svg", obsgs_tr("OBSTitles.AlignLeft"), 0, -1);
    add_align_action("align-horizontal-center.svg", obsgs_tr("OBSTitles.AlignHorizontalCenter"), 1, -1);
    add_align_action("align-right.svg", obsgs_tr("OBSTitles.AlignRight"), 2, -1);
    add_align_action("align-top.svg", obsgs_tr("OBSTitles.AlignTop"), -1, 0);
    add_align_action("align-vertical-center.svg", obsgs_tr("OBSTitles.AlignVerticalCenter"), -1, 1);
    add_align_action("align-bottom.svg", obsgs_tr("OBSTitles.AlignBottom"), -1, 2);
    add_align_action("align-center-artboard.svg", obsgs_tr("OBSTitles.AlignCenterToArtboard"), 1, 1);

    toolbar_->addSeparator();
    auto add_flip_action = [this](const char *icon_name, const QString &text, bool horizontal) {
        QAction *action = toolbar_->addAction(obs_icon(icon_name), text);
        action->setToolTip(text);
        connect(action, &QAction::triggered, this, [this, horizontal]() {
            flip_selected_layers(horizontal);
        });
        return action;
    };
    add_flip_action("flip-horizontal.svg", obsgs_tr("OBSTitles.FlipHorizontal"), true);
    add_flip_action("flip-vertical.svg", obsgs_tr("OBSTitles.FlipVertical"), false);

    toolbar_->addSeparator();
    auto *rotation_degrees = new QDoubleSpinBox(toolbar_);
    rotation_degrees->setRange(-9999.0, 9999.0);
    rotation_degrees->setDecimals(1);
    rotation_degrees->setSingleStep(1.0);
    rotation_degrees->setValue(90.0);
    rotation_degrees->setSuffix(QStringLiteral("°"));
    rotation_degrees->setToolTip(obsgs_tr("OBSTitles.RotateDegreesTooltip"));
    rotation_degrees->setAccessibleName(obsgs_tr("OBSTitles.RotateDegrees"));
    rotation_degrees->setFixedWidth(78);
    rotation_degrees->setStyleSheet("QDoubleSpinBox{color:#ddd;background:#202020;border:1px solid #3f3f3f;border-radius:3px;padding:2px 4px;}"
                                    "QDoubleSpinBox::up-button,QDoubleSpinBox::down-button{width:0;border:none;}");
    toolbar_->addWidget(rotation_degrees);
    auto add_rotate_action = [this, rotation_degrees](const char *icon_name, const QString &text, double direction) {
        QAction *action = toolbar_->addAction(obs_icon(icon_name), text);
        action->setToolTip(text);
        connect(action, &QAction::triggered, this, [this, rotation_degrees, direction]() {
            rotate_selected_layers(rotation_degrees->value() * direction);
        });
        return action;
    };
    add_rotate_action("rotate-left.svg", obsgs_tr("OBSTitles.RotateLeft"), -1.0);
    add_rotate_action("rotate-right.svg", obsgs_tr("OBSTitles.RotateRight"), 1.0);

    act_safe_guides_ = new QAction(obs_icon("safe.svg"), obsgs_tr("OBSTitles.Safe"), this);
    act_safe_guides_->setCheckable(true);
    act_safe_guides_->setToolTip(obsgs_tr("OBSTitles.SafeTooltip"));
    connect(act_safe_guides_, &QAction::toggled, this, [this](bool visible) {
        if (canvas_) canvas_->set_safe_guides_visible(visible);
    });

    toolbar_->addSeparator();
    toolbar_->addAction(act_undo_);
    toolbar_->addAction(act_redo_);
    update_undo_redo_actions();

    auto *toolbar_spacer = new QWidget(toolbar_);
    toolbar_spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    toolbar_->addWidget(toolbar_spacer);

    act_live_editing_ = new QAction(obsgs_tr("OBSTitles.LiveEditing"), this);
    act_live_editing_->setCheckable(true);
    act_live_editing_->setToolTip(obsgs_tr("OBSTitles.LiveEditingTooltip"));
    connect(act_live_editing_, &QAction::toggled, this, &TitleEditor::set_live_editing_enabled);

    auto *live_editing_button = new QToolButton(toolbar_);
    live_editing_button->setDefaultAction(act_live_editing_);
    live_editing_button->setToolButtonStyle(Qt::ToolButtonTextOnly);
    toolbar_->addWidget(live_editing_button);

}


static QString editor_template_library_root_path()
{
    char *path = obs_module_config_path("template-library");
    QString root = path ? QString::fromUtf8(path) : QDir::homePath();
    if (path) bfree(path);
    QDir().mkpath(root);
    return root;
}

static int editor_dialog_layout_spacing(QWidget *widget)
{
    const int spacing = widget->style()->pixelMetric(QStyle::PM_LayoutVerticalSpacing, nullptr, widget);
    return spacing > 0 ? spacing : 6;
}

static QStringList editor_template_library_category_paths(const QString &root_path)
{
    QStringList categories;
    QDirIterator it(root_path, QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        const QString rel = QDir(root_path).relativeFilePath(it.filePath());
        if (!rel.isEmpty()) categories << rel;
    }
    if (categories.isEmpty()) categories << QStringLiteral("Custom");
    categories.sort(Qt::CaseInsensitive);
    return categories;
}

static QString editor_sanitized_template_category_path(const QString &category)
{
    QString cleaned = QDir::cleanPath(category.trimmed());
    cleaned.replace(QRegularExpression(QStringLiteral("[\\\\:*?\"<>|]")), QStringLiteral("_"));
    while (cleaned.startsWith(QStringLiteral("../")))
        cleaned.remove(0, 3);
    if (cleaned == QStringLiteral(".") || cleaned == QStringLiteral(".."))
        cleaned.clear();
    return cleaned;
}

static bool prompt_editor_template_library_category(QWidget *parent, QString &category)
{
    const QString root_path = editor_template_library_root_path();
    QDialog dialog(parent);
    dialog.setWindowTitle(obsgs_tr("OBSTitles.TemplateLibraryCategoryTitle"));
    dialog.setModal(true);

    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(editor_dialog_layout_spacing(&dialog));

    auto *prompt = new QLabel(obsgs_tr("OBSTitles.TemplateLibraryCategoryPrompt"), &dialog);
    prompt->setWordWrap(true);
    layout->addWidget(prompt);

    auto *combo = new QComboBox(&dialog);
    combo->setEditable(true);
    combo->addItems(editor_template_library_category_paths(root_path));
    layout->addWidget(combo);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&]() {
        const QString safe_category = editor_sanitized_template_category_path(combo->currentText());
        if (safe_category.isEmpty()) {
            QMessageBox::warning(&dialog, obsgs_tr("OBSTitles.TemplateLibraryCategoryTitle"),
                                 obsgs_tr("OBSTitles.TemplateLibraryCategoryRequired"));
            return;
        }
        category = safe_category;
        dialog.accept();
    });
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);

    return dialog.exec() == QDialog::Accepted;
}

static bool prompt_editor_template_metadata(QWidget *parent, const Title &title,
                                           TitleTemplateExportMetadata &metadata,
                                           const QString &window_title = obsgs_tr("OBSTitles.ExportTemplateDetails"))
{
    if (metadata.title.empty()) metadata.title = title.name;
    if (metadata.description.empty()) metadata.description = title.description;
    if (metadata.creator.empty()) metadata.creator = title.creator;
    if (metadata.creation_date.empty()) {
        metadata.creation_date = title.creation_date.empty()
            ? QDateTime::currentDateTimeUtc().toString(Qt::ISODate).toStdString()
            : title.creation_date;
    }
    if (metadata.screenshot_png_base64.empty())
        metadata.screenshot_png_base64 = title.preview_screenshot_png_base64;

    QDialog dialog(parent);
    dialog.setWindowTitle(window_title);
    dialog.setModal(true);
    dialog.resize(560, 500);

    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(editor_dialog_layout_spacing(&dialog));

    auto *preview_label = new QLabel(obsgs_tr("OBSTitles.TemplateScreenshotPreviewLabel"), &dialog);
    QFont label_font = preview_label->font();
    label_font.setBold(true);
    preview_label->setFont(label_font);
    layout->addWidget(preview_label);

    auto *preview = new QLabel(&dialog);
    preview->setAlignment(Qt::AlignCenter);
    preview->setFrameStyle(QFrame::StyledPanel | QFrame::Sunken);
    preview->setMinimumHeight(160);
    QPixmap pixmap;
    const QByteArray png = QByteArray::fromBase64(QByteArray::fromStdString(metadata.screenshot_png_base64));
    if (!png.isEmpty() && pixmap.loadFromData(png, "PNG"))
        preview->setPixmap(pixmap.scaled(QSize(480, 180), Qt::KeepAspectRatio, Qt::SmoothTransformation));
    else
        preview->setText(obsgs_tr("OBSTitles.TemplateScreenshotFailed"));
    layout->addWidget(preview);

    auto *form = new QFormLayout();
    auto *title_edit = new QLineEdit(QString::fromStdString(metadata.title), &dialog);
    auto *description_edit = new QTextEdit(&dialog);
    description_edit->setAcceptRichText(false);
    description_edit->setPlainText(QString::fromStdString(metadata.description));
    description_edit->setMinimumHeight(96);
    auto *creator_edit = new QLineEdit(QString::fromStdString(metadata.creator), &dialog);
    auto *date_edit = new QLineEdit(QString::fromStdString(metadata.creation_date), &dialog);

    form->addRow(obsgs_tr("OBSTitles.TemplateExportTitleLabel"), title_edit);
    form->addRow(obsgs_tr("OBSTitles.TemplateExportDescriptionLabel"), description_edit);
    form->addRow(obsgs_tr("OBSTitles.TemplateExportCreatorLabel"), creator_edit);
    form->addRow(obsgs_tr("OBSTitles.TemplateCreationDateLabel"), date_edit);
    layout->addLayout(form);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&]() {
        if (title_edit->text().trimmed().isEmpty()) {
            QMessageBox::warning(&dialog, obsgs_tr("OBSTitles.ExportTemplateDetails"),
                                 obsgs_tr("OBSTitles.TemplateExportTitleRequired"));
            return;
        }
        dialog.accept();
    });
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);

    if (dialog.exec() != QDialog::Accepted)
        return false;

    metadata.title = title_edit->text().trimmed().toStdString();
    metadata.description = description_edit->toPlainText().trimmed().toStdString();
    metadata.creator = creator_edit->text().trimmed().toStdString();
    metadata.creation_date = date_edit->text().trimmed().toStdString();
    return true;
}

void TitleEditor::copy_title_to_store(const std::shared_ptr<Title> &source,
                                      const std::shared_ptr<Title> &dest) const
{
    if (!source || !dest) return;
    const std::string dest_id = dest->id;
    *dest = *source;
    dest->id = dest_id;
    dest->layers.clear();
    dest->layers.reserve(source->layers.size());
    for (const auto &layer : source->layers) {
        if (layer) dest->layers.push_back(std::make_shared<Layer>(*layer));
    }
}

void TitleEditor::new_title_contents()
{
    if (!title_) return;
    if (QMessageBox::question(this, obsgs_tr("OBSTitles.New"),
                              obsgs_tr("OBSTitles.NewTitleConfirm")) != QMessageBox::Yes)
        return;

    title_->layers.clear();
    sel_layer_id_.clear();
    layers_->refresh();
    canvas_->set_selected_layers({});
    props_->set_layer(nullptr, playhead_);
    on_title_modified();
}

bool TitleEditor::persist_title_changes(bool update_preview_screenshot, bool show_saved_status)
{
    if (!title_) return false;
    auto stored = TitleDataStore::instance().get_title(editing_title_id_.empty() ? title_->id : editing_title_id_);
    if (!stored) {
        stored = TitleDataStore::instance().create_title(title_->name);
        editing_title_id_ = stored->id;
        title_->id = stored->id;
    }
    copy_title_to_store(title_, stored);
    if (update_preview_screenshot) {
        title_->preview_screenshot_png_base64 = title_manual_screenshot_png_base64(*title_);
        stored->preview_screenshot_png_base64 = title_->preview_screenshot_png_base64;
    }
    TitleDataStore::instance().notify_change();
    TitleDataStore::instance().save();
    emit title_saved(stored->id);
    set_dirty(false);
    if (show_saved_status)
        setWindowTitle(obsgs_tr("OBSTitles.EditorSavedTitle"));
    return true;
}

bool TitleEditor::save_title()
{
    return persist_title_changes(true, true);
}

void TitleEditor::set_live_editing_enabled(bool enabled)
{
    live_editing_ = enabled;
    if (act_live_editing_ && act_live_editing_->isChecked() != enabled) {
        QSignalBlocker blocker(act_live_editing_);
        act_live_editing_->setChecked(enabled);
    }
    if (live_editing_ && dirty_)
        save_live_edit();
}

void TitleEditor::save_live_edit()
{
    if (!live_editing_ || !title_) return;
    persist_title_changes(false, false);
}

void TitleEditor::save_title_as_new()
{
    if (!title_) return;

    Title temp = *title_;
    temp.preview_screenshot_png_base64 = title_manual_screenshot_png_base64(temp);
    TitleTemplateExportMetadata metadata;
    metadata.screenshot_png_base64 = temp.preview_screenshot_png_base64;
    if (!prompt_editor_template_metadata(this, temp, metadata, obsgs_tr("OBSTitles.SaveAsNew")))
        return;

    auto created = TitleDataStore::instance().create_title(metadata.title);
    title_->name = metadata.title;
    title_->description = metadata.description;
    title_->creator = metadata.creator;
    title_->creation_date = metadata.creation_date;
    title_->preview_screenshot_png_base64 = metadata.screenshot_png_base64;
    copy_title_to_store(title_, created);
    created->name = metadata.title;
    created->description = metadata.description;
    created->creator = metadata.creator;
    created->creation_date = metadata.creation_date;
    created->preview_screenshot_png_base64 = metadata.screenshot_png_base64;
    editing_title_id_ = created->id;
    title_->id = created->id;
    update_title_bar();
    TitleDataStore::instance().notify_change();
    TitleDataStore::instance().save();
    emit title_saved(created->id);
    set_dirty(false);
    setWindowTitle(obsgs_tr("OBSTitles.EditorSavedTitle"));
}

void TitleEditor::export_title_template(bool save_in_library)
{
    if (!title_) return;

    Title temp = *title_;
    temp.preview_screenshot_png_base64 = title_manual_screenshot_png_base64(temp);
    TitleTemplateExportMetadata metadata;
    metadata.screenshot_png_base64 = temp.preview_screenshot_png_base64;
    if (!prompt_editor_template_metadata(this, temp, metadata))
        return;

    QString safe_name = QString::fromStdString(metadata.title).trimmed();
    if (safe_name.isEmpty()) safe_name = obsgs_tr("OBSTitles.TemplateFileDialogTitle");
    safe_name.replace(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|]")), QStringLiteral("_"));

    QString path;
    if (save_in_library) {
        QString category;
        if (!prompt_editor_template_library_category(this, category))
            return;
        QDir root(editor_template_library_root_path());
        root.mkpath(category);
        path = root.filePath(QStringLiteral("%1/%2.ogspt").arg(category, safe_name));
    } else {
        path = QFileDialog::getSaveFileName(this, obsgs_tr("OBSTitles.ExportTitleTemplate"),
                                            QDir(editor_template_library_root_path()).filePath(safe_name + QStringLiteral(".ogspt")),
                                            obsgs_tr("OBSTitles.TemplateFileFilter"));
        if (path.isEmpty()) return;
        if (QFileInfo(path).suffix().isEmpty()) path += QStringLiteral(".ogspt");
    }

    auto stored = TitleDataStore::instance().create_title(metadata.title);
    copy_title_to_store(title_, stored);
    stored->name = metadata.title;
    stored->description = metadata.description;
    stored->creator = metadata.creator;
    stored->creation_date = metadata.creation_date;
    stored->preview_screenshot_png_base64 = metadata.screenshot_png_base64;

    std::string error;
    if (!TitleDataStore::instance().export_title(stored->id, path.toStdString(), metadata, &error)) {
        QMessageBox::warning(this, obsgs_tr("OBSTitles.ExportTitleTemplate"), QString::fromStdString(error));
    } else {
        QMessageBox::information(this, obsgs_tr("OBSTitles.ExportTitleTemplate"),
                                 obsgs_tr("OBSTitles.ExportedStatusFormat").arg(QFileInfo(path).fileName()));
    }
    TitleDataStore::instance().delete_title(stored->id);
}

/* ── open_title ──────────────────────────────────────────────────── */
void TitleEditor::open_title(const std::string &tid)
{
    play_timer_->stop();
    playing_ = false;
    act_play_->setText("▶");
    act_play_->setIcon(obs_icon("play.svg"));
    playhead_ = 0.0;
    playback_reverse_ = false;
    full_loop_playback_ = false;

    auto stored_title = TitleDataStore::instance().get_title(tid);
    if (!stored_title) return;
    editing_title_id_ = tid;
    title_ = clone_title(*stored_title);

    update_title_bar();
    canvas_->set_title(title_);
    layers_->set_title(title_);
    layers_->set_layer_clipboard_available(layer_clipboard_ != nullptr);
    timeline_->set_title(title_);
    props_->set_title(title_);
    title_props_->set_title(title_);

    if (!title_->layers.empty())
        on_layer_selected(title_->layers.back()->id);
    else
        props_->set_layer(nullptr, playhead_);

    QTimer::singleShot(0, timeline_, [this]() {
        if (timeline_) timeline_->fit_timeline();
    });

    undo_stack_.clear();
    undo_index_ = -1;
    push_undo_snapshot();
    update_undo_redo_actions();

    on_playhead_changed(0.0);
    set_dirty(false);
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


std::shared_ptr<Layer> TitleEditor::clone_layer_for_insert(const Layer &layer, bool suffix_name) const
{
    auto clone = std::make_shared<Layer>(layer);
    clone->id = TitleDataStore::make_uuid();
    if (suffix_name)
        clone->name = clone->name.empty() ? editor_text_std("OBSTitles.LayerCopy") : clone->name + editor_text_std("OBSTitles.CopySuffix");
    if (!clone->parent_id.empty() && (!title_ || !title_->find_layer(clone->parent_id)))
        clone->parent_id.clear();
    return clone;
}

void TitleEditor::insert_layer_above(const std::string &anchor_id, std::shared_ptr<Layer> layer)
{
    if (!title_ || !layer) return;

    auto it = std::find_if(title_->layers.begin(), title_->layers.end(),
                           [&](const auto &candidate) {
                               return candidate && candidate->id == anchor_id;
                           });
    if (it == title_->layers.end())
        title_->layers.push_back(layer);
    else
        title_->layers.insert(it + 1, layer);
}

void TitleEditor::select_after_layer_list_mutation(const std::string &layer_id)
{
    layers_->refresh();
    timeline_->set_title(title_);
    on_layer_selected(layer_id);
}


void TitleEditor::copy_selected_layer()
{
    if (!title_ || sel_layer_id_.empty()) return;
    auto layer = title_->find_layer(sel_layer_id_);
    if (!layer) return;
    layer_clipboard_ = std::make_shared<Layer>(*layer);
    if (layers_) layers_->set_layer_clipboard_available(true);
}

void TitleEditor::paste_layer_from_clipboard()
{
    if (!title_ || !layer_clipboard_) return;
    std::string anchor_id = sel_layer_id_;
    auto pasted = clone_layer_for_insert(*layer_clipboard_, true);
    std::string pasted_id = pasted->id;
    insert_layer_above(anchor_id, pasted);
    select_after_layer_list_mutation(pasted_id);
    on_title_modified();
}

void TitleEditor::delete_selected_layer()
{
    if (!title_ || sel_layer_id_.empty()) return;
    std::string removed_id = sel_layer_id_;
    title_->remove_layer(removed_id);
    sel_layer_id_.clear();
    layers_->refresh();

    if (!title_->layers.empty())
        on_layer_selected(title_->layers.back()->id);
    else
        props_->set_layer(nullptr, playhead_);

    on_title_modified();
}

void TitleEditor::cut_selected_layer()
{
    copy_selected_layer();
    delete_selected_layer();
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
    title_->description = snapshot->description;
    title_->creator = snapshot->creator;
    title_->creation_date = snapshot->creation_date;
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
    title_->live_text_column_order = snapshot->live_text_column_order;
    title_->live_text_header_state = snapshot->live_text_header_state;
    title_->external_data_enabled = snapshot->external_data_enabled;
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
    restoring_undo_ = false;
    update_undo_redo_actions();
    set_dirty(true);
    save_live_edit();
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
    if (dirty_indicator_)
        dirty_indicator_->setVisible(dirty_);
}

void TitleEditor::set_dirty(bool dirty)
{
    dirty_ = dirty;
    if (dirty_indicator_)
        dirty_indicator_->setVisible(dirty_);
    setWindowTitle(obsgs_tr(dirty_ ? "OBSTitles.EditorModifiedTitle" : "OBSTitles.EditorWindowTitle"));
}

bool TitleEditor::confirm_save_before_close()
{
    if (!dirty_)
        return true;

    QMessageBox dialog(this);
    dialog.setIcon(QMessageBox::Warning);
    dialog.setWindowTitle(obsgs_tr("OBSTitles.UnsavedChangesTitle"));
    dialog.setText(obsgs_tr("OBSTitles.UnsavedChangesPrompt"));
    dialog.setStandardButtons(QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel);
    dialog.setDefaultButton(QMessageBox::Yes);
    dialog.setEscapeButton(QMessageBox::Cancel);

    const auto result = static_cast<QMessageBox::StandardButton>(dialog.exec());
    if (result == QMessageBox::Yes)
        return save_title();
    if (result == QMessageBox::No)
        return true;
    return false;
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
        act_play_->setIcon(obs_icon("pause.svg"));
        playback_clock_.restart();
        play_timer_->start();
    } else {
        act_play_->setText("▶");
        act_play_->setIcon(obs_icon("play.svg"));
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
    act_play_->setIcon(obs_icon("pause.svg"));
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
                act_play_->setIcon(obs_icon("play.svg"));
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
                act_play_->setIcon(obs_icon("play.svg"));
            }
            break;
        }
    }
    on_playhead_changed(snap_to_obs_frame(t));
}

static bool editor_focus_accepts_text(QWidget *widget)
{
    return qobject_cast<QLineEdit *>(widget) ||
           qobject_cast<QTextEdit *>(widget) ||
           qobject_cast<QAbstractSpinBox *>(widget) ||
           qobject_cast<QComboBox *>(widget);
}

void TitleEditor::show_about()
{
    QMessageBox::about(
        this,
        obsgs_tr("OBSTitles.AboutTitle"),
        obsgs_tr("OBSTitles.AboutTextFormat").arg(QStringLiteral(PLUGIN_VERSION)));
}


bool TitleEditor::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::KeyPress && isActiveWindow()) {
        auto *key_event = static_cast<QKeyEvent *>(event);
        auto *widget = qobject_cast<QWidget *>(watched);
        const bool in_editor = widget && (widget == this || isAncestorOf(widget));
        if (in_editor && key_event->key() == Qt::Key_Space && !key_event->isAutoRepeat()) {
            if (!editor_focus_accepts_text(focusWidget())) {
                play_pause();
                key_event->accept();
                return true;
            }
        }
    }
    return QDialog::eventFilter(watched, event);
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
    QWidget *fw = focusWidget();
    bool editing_value = editor_focus_accepts_text(fw);
    if (!editing_value && timeline_ && ev->matches(QKeySequence::Copy) &&
        timeline_->has_selected_keyframes()) {
        timeline_->copy_keyframe_selection();
        ev->accept();
        return;
    }
    if (!editing_value && timeline_ && ev->matches(QKeySequence::Cut) &&
        timeline_->has_selected_keyframes()) {
        timeline_->cut_keyframe_selection();
        ev->accept();
        return;
    }
    if (!editing_value && timeline_ && ev->matches(QKeySequence::Paste) &&
        timeline_->has_keyframe_clipboard()) {
        timeline_->paste_keyframes_at_playhead();
        ev->accept();
        return;
    }
    if (!editing_value && timeline_ &&
        (ev->key() == Qt::Key_Delete || ev->key() == Qt::Key_Backspace) &&
        timeline_->has_selected_keyframes()) {
        timeline_->delete_keyframe_selection();
        ev->accept();
        return;
    }
    if (!editing_value && ev->matches(QKeySequence::Copy) && !sel_layer_id_.empty()) {
        copy_selected_layer();
        ev->accept();
        return;
    }
    if (!editing_value && ev->matches(QKeySequence::Cut) && !sel_layer_id_.empty()) {
        cut_selected_layer();
        ev->accept();
        return;
    }
    if (!editing_value && ev->matches(QKeySequence::Paste) && layer_clipboard_) {
        paste_layer_from_clipboard();
        ev->accept();
        return;
    }
    if (!editing_value && ev->key() == Qt::Key_Delete && !sel_layer_id_.empty()) {
        delete_selected_layer();
        ev->accept();
        return;
    }
    if (ev->key() == Qt::Key_Space && !ev->isAutoRepeat()) {
        if (!editor_focus_accepts_text(focusWidget())) {
            play_pause();
            ev->accept();
            return;
        }
    }
    QDialog::keyPressEvent(ev);
}


void TitleEditor::closeEvent(QCloseEvent *ev)
{
    if (confirm_save_before_close())
        ev->accept();
    else
        ev->ignore();
}

void TitleEditor::reject()
{
    if (confirm_save_before_close())
        QDialog::reject();
}

/* ── Signal handlers ─────────────────────────────────────────────── */
void TitleEditor::on_layer_selected(const std::string &lid)
{
    sel_layer_id_ = lid;
    layers_->set_selected_layer(lid);
    canvas_->set_selected_layer(lid);
    timeline_->set_selected_layer(lid);

    if (!title_ || lid.empty()) {
        props_->set_layer(nullptr, playhead_);
        return;
    }
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
        time_lbl_->setText(obsgs_tr("OBSTitles.TimeFpsFormat").arg(format_timecode(t)).arg(obs_frame_rate(), 0, 'f', 2));
}

void TitleEditor::on_title_modified(bool push_undo)
{
    if (title_) set_dirty(true);
    canvas_->refresh_preview();
    if (title_props_) title_props_->set_title(title_);
    if (timeline_) timeline_->set_title(title_);
    if (push_undo)
        push_undo_snapshot();
    save_live_edit();
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
    setFocusPolicy(Qt::StrongFocus);
}

void CanvasPreview::set_title(std::shared_ptr<Title> t)
{
    title_ = t;
    dirty_ = true;
    pan_offset_ = QPointF(0, 0);
    if (title_) fit_canvas(fit_zoom_up_to_100_);
    else update();
}

void CanvasPreview::set_playhead(double t)
{
    playhead_ = t; dirty_ = true; update();
}

void CanvasPreview::set_selected_layer(const std::string &lid)
{
    sel_layer_id_ = lid;
    selected_layer_ids_.clear();
    if (!lid.empty()) selected_layer_ids_.push_back(lid);
    update();
}

void CanvasPreview::set_selected_layers(const std::vector<std::string> &ids)
{
    selected_layer_ids_ = ids;
    sel_layer_id_ = ids.empty() ? std::string() : ids.back();
    update();
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


void CanvasPreview::set_snap_enabled(bool enabled)
{
    snap_settings_.enabled = enabled;
    if (!enabled) clear_snap_feedback();
    update();
}

void CanvasPreview::set_snap_to_guides(bool enabled)
{
    snap_settings_.guides = enabled;
    if (!enabled) clear_snap_feedback();
    update();
}

void CanvasPreview::set_snap_to_grid(bool enabled)
{
    snap_settings_.grid = enabled;
    if (!enabled) clear_snap_feedback();
    update();
}

void CanvasPreview::set_snap_to_object_edges(bool enabled)
{
    snap_settings_.object_edges = enabled;
    if (!enabled) clear_snap_feedback();
    update();
}

void CanvasPreview::set_snap_to_object_centers(bool enabled)
{
    snap_settings_.object_centers = enabled;
    if (!enabled) clear_snap_feedback();
    update();
}

void CanvasPreview::set_snap_to_canvas_bounds(bool enabled)
{
    snap_settings_.canvas_bounds = enabled;
    if (!enabled) clear_snap_feedback();
    update();
}

void CanvasPreview::set_snap_to_spacing(bool enabled)
{
    snap_settings_.spacing = enabled;
    if (!enabled) clear_snap_feedback();
    update();
}

void CanvasPreview::set_zoom_percent(int percent)
{
    int clamped = std::clamp(percent, 5, 1600);
    if (zoom_percent_ == clamped && !fit_zoom_active_) return;
    zoom_percent_ = clamped;
    fit_zoom_active_ = false;
    emit zoom_percent_changed(zoom_percent_);
    update();
}

int CanvasPreview::zoom_percent() const
{
    return zoom_percent_;
}

void CanvasPreview::set_checkerboard_pattern(int pattern)
{
    checkerboard_pattern_ = std::clamp(pattern, 0, 2);
    update();
}

void CanvasPreview::fit_canvas(bool up_to_100)
{
    fit_zoom_active_ = true;
    fit_zoom_up_to_100_ = up_to_100;
    pan_offset_ = QPointF(0, 0);
    double scale = fit_scale();
    if (up_to_100) scale = std::min(scale, 1.0);
    int next_percent = std::clamp((int)std::round(scale * 100.0), 5, 1600);
    if (zoom_percent_ != next_percent) {
        zoom_percent_ = next_percent;
        emit zoom_percent_changed(zoom_percent_);
    }
    update();
}

std::shared_ptr<Layer> CanvasPreview::selected_layer() const
{
    return title_ ? title_->find_layer(sel_layer_id_) : nullptr;
}

std::vector<std::shared_ptr<Layer>> CanvasPreview::selected_layers() const
{
    std::vector<std::shared_ptr<Layer>> layers;
    if (!title_) return layers;
    if (selected_layer_ids_.empty()) {
        if (auto layer = selected_layer()) layers.push_back(layer);
        return layers;
    }
    std::set<std::string> seen;
    for (const auto &id : selected_layer_ids_) {
        if (!seen.insert(id).second) continue;
        auto layer = title_->find_layer(id);
        if (layer) layers.push_back(layer);
    }
    return layers;
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

double CanvasPreview::fit_scale() const
{
    if (!title_ || title_->width <= 0 || title_->height <= 0) return 1.0;
    return std::min((double)width() / title_->width,
                    (double)height() / title_->height);
}

double CanvasPreview::view_scale() const
{
    return std::max(0.05, (double)zoom_percent_ / 100.0);
}

QPointF CanvasPreview::centered_view_origin() const
{
    if (!title_) return QPointF(0, 0);
    double scale = view_scale();
    return QPointF((width() - title_->width * scale) / 2.0,
                   (height() - title_->height * scale) / 2.0);
}

QPointF CanvasPreview::view_origin() const
{
    return centered_view_origin() + pan_offset_;
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
    auto non_zero_scale = [](double value) {
        if (std::abs(value) >= 0.0001) return value;
        return value < 0.0 ? -0.0001 : 0.0001;
    };
    double sx = non_zero_scale(layer.scale_x.evaluate(lt));
    double sy = non_zero_scale(layer.scale_y.evaluate(lt));
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

QRectF CanvasPreview::layer_canvas_bounds(const Layer &layer) const
{
    QRectF r = layer_local_rect(layer);
    const QPointF corners[] = {r.topLeft(), r.topRight(), r.bottomRight(), r.bottomLeft()};

    double min_x = std::numeric_limits<double>::infinity();
    double min_y = std::numeric_limits<double>::infinity();
    double max_x = -std::numeric_limits<double>::infinity();
    double max_y = -std::numeric_limits<double>::infinity();

    for (const QPointF &corner : corners) {
        QPointF canvas = layer_to_canvas(layer, corner);
        min_x = std::min(min_x, canvas.x());
        min_y = std::min(min_y, canvas.y());
        max_x = std::max(max_x, canvas.x());
        max_y = std::max(max_y, canvas.y());
    }

    if (!std::isfinite(min_x) || !std::isfinite(min_y) ||
        !std::isfinite(max_x) || !std::isfinite(max_y))
        return QRectF();

    return QRectF(QPointF(min_x, min_y), QPointF(max_x, max_y)).normalized();
}

QRectF CanvasPreview::selected_canvas_bounds() const
{
    QRectF bounds;
    bool have_bounds = false;
    for (auto &layer : selected_layers()) {
        if (!layer || !layer->visible) continue;
        QRectF layer_bounds = layer_canvas_bounds(*layer);
        if (!layer_bounds.isValid()) continue;
        if (!have_bounds) {
            bounds = layer_bounds;
            have_bounds = true;
        } else {
            bounds = bounds.united(layer_bounds);
        }
    }
    return bounds.normalized();
}

CanvasPreview::DragMode CanvasPreview::hit_test_selected(const QPointF &view_pt) const
{
    auto layers = selected_layers();
    if (layers.empty()) return DragMode::None;

    if (layers.size() > 1) {
        QRectF r = selected_canvas_bounds();
        if (!r.isValid() || r.isEmpty()) return DragMode::None;
        auto canvas_handle_to_view = [&](const QPointF &p) { return canvas_to_view(p); };
        auto near_pt = [&](const QPointF &p) {
            QPointF view = canvas_handle_to_view(p);
            return std::abs(view_pt.x() - view.x()) <= CANVAS_CONTROL_HIT_RADIUS_PX &&
                   std::abs(view_pt.y() - view.y()) <= CANVAS_CONTROL_HIT_RADIUS_PX;
        };
        QRectF view_bounds(canvas_to_view(r.topLeft()), canvas_to_view(r.bottomRight()));
        view_bounds = view_bounds.normalized();
        auto near_rotation_corner = [&](const QPointF &p) {
            return QLineF(view_pt, canvas_handle_to_view(p)).length() <= CANVAS_ROTATE_HIT_RADIUS_PX &&
                   !view_bounds.adjusted(-CANVAS_CONTROL_SIZE_PX, -CANVAS_CONTROL_SIZE_PX,
                                         CANVAS_CONTROL_SIZE_PX, CANVAS_CONTROL_SIZE_PX).contains(view_pt);
        };
        if (near_pt(r.topLeft())) return DragMode::ResizeNW;
        if (near_pt(QPointF(r.center().x(), r.top()))) return DragMode::ResizeN;
        if (near_pt(r.topRight())) return DragMode::ResizeNE;
        if (near_pt(QPointF(r.right(), r.center().y()))) return DragMode::ResizeE;
        if (near_pt(r.bottomRight())) return DragMode::ResizeSE;
        if (near_pt(QPointF(r.center().x(), r.bottom()))) return DragMode::ResizeS;
        if (near_pt(r.bottomLeft())) return DragMode::ResizeSW;
        if (near_pt(QPointF(r.left(), r.center().y()))) return DragMode::ResizeW;
        if (near_rotation_corner(r.topLeft()) || near_rotation_corner(r.topRight()) ||
            near_rotation_corner(r.bottomRight()) || near_rotation_corner(r.bottomLeft()))
            return DragMode::Rotate;
        QPointF canvas = view_to_canvas(view_pt);
        for (const auto &layer : layers) {
            if (!layer || layer->locked) continue;
            QPointF local = canvas_to_layer(*layer, canvas);
            if (layer_local_rect(*layer).contains(local))
                return DragMode::Move;
        }
        return DragMode::None;
    }

    auto layer = layers.front();
    if (!layer || layer->locked) return DragMode::None;

    QRectF r = layer_local_rect(*layer);
    auto layer_point_to_view = [&](const QPointF &p) {
        return canvas_to_view(layer_to_canvas(*layer, p));
    };
    auto near_pt = [&](const QPointF &p) {
        QPointF view = layer_point_to_view(p);
        return std::abs(view_pt.x() - view.x()) <= CANVAS_CONTROL_HIT_RADIUS_PX &&
               std::abs(view_pt.y() - view.y()) <= CANVAS_CONTROL_HIT_RADIUS_PX;
    };
    QPainterPath layer_path;
    layer_path.moveTo(layer_point_to_view(r.topLeft()));
    layer_path.lineTo(layer_point_to_view(r.topRight()));
    layer_path.lineTo(layer_point_to_view(r.bottomRight()));
    layer_path.lineTo(layer_point_to_view(r.bottomLeft()));
    layer_path.closeSubpath();
    auto near_rotation_corner = [&](const QPointF &p) {
        return QLineF(view_pt, layer_point_to_view(p)).length() <= CANVAS_ROTATE_HIT_RADIUS_PX &&
               !layer_path.contains(view_pt);
    };

    if (near_pt(r.topLeft())) return DragMode::ResizeNW;
    if (near_pt(QPointF(r.center().x(), r.top()))) return DragMode::ResizeN;
    if (near_pt(r.topRight())) return DragMode::ResizeNE;
    if (near_pt(QPointF(r.right(), r.center().y()))) return DragMode::ResizeE;
    if (near_pt(r.bottomRight())) return DragMode::ResizeSE;
    if (near_pt(QPointF(r.center().x(), r.bottom()))) return DragMode::ResizeS;
    if (near_pt(r.bottomLeft())) return DragMode::ResizeSW;
    if (near_pt(QPointF(r.left(), r.center().y()))) return DragMode::ResizeW;
    if (near_rotation_corner(r.topLeft()) || near_rotation_corner(r.topRight()) ||
        near_rotation_corner(r.bottomRight()) || near_rotation_corner(r.bottomLeft()))
        return DragMode::Rotate;
    if (QLineF(view_pt, layer_point_to_view(QPointF(0, 0))).length() <= CANVAS_CONTROL_HIT_RADIUS_PX * 1.25)
        return DragMode::Origin;

    if (layer_path.contains(view_pt)) return DragMode::Move;
    return DragMode::None;
}
void CanvasPreview::begin_marquee(const QPointF &view_pt, Qt::KeyboardModifiers)
{
    drag_mode_ = DragMode::Marquee;
    marquee_active_ = false;
    drag_start_view_ = view_pt;
    drag_current_view_ = view_pt;
    marquee_base_selection_ = selected_layer_ids_;
    drag_changed_ = false;
    clear_snap_feedback();
}

void CanvasPreview::update_marquee(const QPointF &view_pt, Qt::KeyboardModifiers modifiers)
{
    if (!title_) return;
    drag_current_view_ = view_pt;
    if ((drag_current_view_ - drag_start_view_).manhattanLength() < QApplication::startDragDistance()) {
        update();
        return;
    }

    marquee_active_ = true;
    QRectF view_rect(drag_start_view_, drag_current_view_);
    view_rect = view_rect.normalized();
    QRectF canvas_rect(view_to_canvas(view_rect.topLeft()), view_to_canvas(view_rect.bottomRight()));
    canvas_rect = canvas_rect.normalized();
    auto intersects_or_touches = [](const QRectF &a, const QRectF &b) {
        if (!a.isValid() || !b.isValid()) return false;
        return a.left() <= b.right() && a.right() >= b.left() &&
               a.top() <= b.bottom() && a.bottom() >= b.top();
    };

    std::set<std::string> selected;
    if (modifiers & (Qt::ShiftModifier | Qt::ControlModifier))
        selected.insert(marquee_base_selection_.begin(), marquee_base_selection_.end());

    std::vector<std::string> hits;
    for (const auto &layer : title_->layers) {
        if (!layer || !layer->visible || layer->locked) continue;
        if (playhead_ < layer->in_time || playhead_ > layer->out_time) continue;
        QRectF bounds = layer_canvas_bounds(*layer);
        if (intersects_or_touches(canvas_rect, bounds))
            hits.push_back(layer->id);
    }

    if (modifiers & Qt::ControlModifier) {
        for (const auto &id : hits) {
            auto it = selected.find(id);
            if (it == selected.end()) selected.insert(id);
            else selected.erase(it);
        }
    } else {
        selected.insert(hits.begin(), hits.end());
    }

    selected_layer_ids_.clear();
    for (const auto &layer : title_->layers) {
        if (layer && selected.find(layer->id) != selected.end())
            selected_layer_ids_.push_back(layer->id);
    }
    sel_layer_id_ = selected_layer_ids_.empty() ? std::string() : selected_layer_ids_.back();
    emit layers_selected(selected_layer_ids_);
    update();
}

bool CanvasPreview::duplicate_selected_layers_for_drag()
{
    if (!title_ || selected_layer_ids_.empty()) return false;

    std::set<std::string> selected_ids(selected_layer_ids_.begin(), selected_layer_ids_.end());
    std::map<std::string, std::string> cloned_ids_by_original;
    std::map<std::string, std::shared_ptr<Layer>> clones_by_original;
    std::vector<std::shared_ptr<Layer>> clones;

    for (const auto &layer : title_->layers) {
        if (!layer || layer->locked || selected_ids.find(layer->id) == selected_ids.end())
            continue;

        auto clone = std::make_shared<Layer>(*layer);
        clone->id = TitleDataStore::make_uuid();
        clone->name = clone->name.empty() ? editor_text_std("OBSTitles.LayerCopy") :
                                            clone->name + editor_text_std("OBSTitles.CopySuffix");
        cloned_ids_by_original[layer->id] = clone->id;
        clones_by_original[layer->id] = clone;
        clones.push_back(clone);
    }

    if (clones.empty()) return false;

    for (auto &clone : clones) {
        auto parent_clone = cloned_ids_by_original.find(clone->parent_id);
        if (parent_clone != cloned_ids_by_original.end()) {
            clone->parent_id = parent_clone->second;
        } else if (!clone->parent_id.empty() && !title_->find_layer(clone->parent_id)) {
            clone->parent_id.clear();
        }
    }

    std::vector<std::shared_ptr<Layer>> next_layers;
    next_layers.reserve(title_->layers.size() + clones.size());
    for (const auto &layer : title_->layers) {
        next_layers.push_back(layer);
        if (!layer) continue;
        auto clone = clones_by_original.find(layer->id);
        if (clone != clones_by_original.end())
            next_layers.push_back(clone->second);
    }
    title_->layers = std::move(next_layers);

    selected_layer_ids_.clear();
    drag_layer_states_.clear();
    for (const auto &clone : clones) {
        if (!clone) continue;
        selected_layer_ids_.push_back(clone->id);
        double lt = std::clamp(playhead_ - clone->in_time, 0.0,
                               std::max(0.0, clone->out_time - clone->in_time));
        drag_layer_states_.push_back({clone->id,
                                      clone->pos_x.evaluate(lt),
                                      clone->pos_y.evaluate(lt),
                                      (float)eval_box_width(*clone, lt),
                                      (float)eval_box_height(*clone, lt)});
    }
    sel_layer_id_ = selected_layer_ids_.empty() ? std::string() : selected_layer_ids_.back();
    drag_start_selection_bounds_ = selected_canvas_bounds();

    emit layer_structure_changed();
    emit layers_selected(selected_layer_ids_);
    dirty_ = true;
    update();
    return true;
}

bool CanvasPreview::nudge_selected_layers(double dx, double dy)
{
    auto layers = selected_layers();
    if (!title_ || layers.empty()) return false;

    bool changed = false;
    for (const auto &layer : layers) {
        if (!layer || layer->locked) continue;
        double lt = std::clamp(playhead_ - layer->in_time, 0.0,
                               std::max(0.0, layer->out_time - layer->in_time));
        set_animated_value(layer->pos_x, lt, layer->pos_x.evaluate(lt) + dx);
        set_animated_value(layer->pos_y, lt, layer->pos_y.evaluate(lt) + dy);
        changed = true;
    }

    if (!changed) return false;

    dirty_ = true;
    update();
    emit layer_geometry_changed();
    return true;
}


void CanvasPreview::clear_snap_feedback()
{
    if (snap_feedback_.empty()) return;
    snap_feedback_.clear();
    update();
}

void CanvasPreview::add_snap_feedback(bool x_axis, double value, const QString &label)
{
    snap_feedback_.push_back({x_axis, value, label});
}

void CanvasPreview::collect_snap_targets(bool x_axis, std::vector<double> &targets, std::vector<QString> &labels) const
{
    if (!title_) return;
    auto add = [&](double value, const QString &label) {
        if (!std::isfinite(value)) return;
        targets.push_back(value);
        labels.push_back(label);
    };

    if (snap_settings_.canvas_bounds) {
        const double size = x_axis ? title_->width : title_->height;
        add(0.0, QStringLiteral("Canvas"));
        add(size * 0.5, QStringLiteral("Canvas center"));
        add(size, QStringLiteral("Canvas"));
    }

    if (snap_settings_.guides) {
        const double size = x_axis ? title_->width : title_->height;
        add(size * OBS_ACTION_SAFE_PERCENT, QStringLiteral("Action safe"));
        add(size * (1.0 - OBS_ACTION_SAFE_PERCENT), QStringLiteral("Action safe"));
        add(size * OBS_GRAPHICS_SAFE_PERCENT, QStringLiteral("Title safe"));
        add(size * (1.0 - OBS_GRAPHICS_SAFE_PERCENT), QStringLiteral("Title safe"));
    }

    if (snap_settings_.grid) {
        const double size = x_axis ? title_->width : title_->height;
        constexpr double grid = 10.0;
        for (double v = 0.0; v <= size + 0.01; v += grid)
            add(v, QStringLiteral("Grid"));
    }

    if (!snap_settings_.object_edges && !snap_settings_.object_centers) return;

    for (const auto &layer : title_->layers) {
        if (!layer || !layer->visible) continue;
        if (playhead_ < layer->in_time || playhead_ > layer->out_time) continue;
        if (std::find(selected_layer_ids_.begin(), selected_layer_ids_.end(), layer->id) != selected_layer_ids_.end())
            continue;
        QRectF bounds = layer_canvas_bounds(*layer);
        if (!bounds.isValid() || bounds.isEmpty()) continue;
        if (snap_settings_.object_edges) {
            add(x_axis ? bounds.left() : bounds.top(), QStringLiteral("Object edge"));
            add(x_axis ? bounds.right() : bounds.bottom(), QStringLiteral("Object edge"));
        }
        if (snap_settings_.object_centers)
            add(x_axis ? bounds.center().x() : bounds.center().y(), QStringLiteral("Object center"));
    }
}

void CanvasPreview::collect_spacing_targets(bool x_axis, std::vector<double> &targets, std::vector<QString> &labels) const
{
    if (!title_ || !snap_settings_.spacing) return;

    struct Span { double start; double end; };
    std::vector<Span> spans;
    for (const auto &layer : title_->layers) {
        if (!layer || !layer->visible) continue;
        if (playhead_ < layer->in_time || playhead_ > layer->out_time) continue;
        if (std::find(selected_layer_ids_.begin(), selected_layer_ids_.end(), layer->id) != selected_layer_ids_.end())
            continue;
        QRectF bounds = layer_canvas_bounds(*layer);
        if (!bounds.isValid() || bounds.isEmpty()) continue;
        spans.push_back({x_axis ? bounds.left() : bounds.top(), x_axis ? bounds.right() : bounds.bottom()});
    }
    if (spans.empty()) return;
    std::sort(spans.begin(), spans.end(), [](const Span &a, const Span &b) { return a.start < b.start; });

    std::vector<double> gaps;
    for (size_t i = 1; i < spans.size(); ++i) {
        double gap = spans[i].start - spans[i - 1].end;
        if (gap >= 0.0) gaps.push_back(gap);
    }
    if (gaps.empty()) gaps.push_back(0.0);

    auto add = [&](double value) {
        if (!std::isfinite(value)) return;
        targets.push_back(value);
        labels.push_back(QStringLiteral("Spacing"));
    };
    for (const Span &span : spans) {
        for (double gap : gaps) {
            add(span.start - gap);
            add(span.end + gap);
        }
    }
}

QPointF CanvasPreview::snap_delta_for_bounds(const QRectF &start_bounds, const QPointF &delta, bool snap_x, bool snap_y)
{
    if (!title_ || !snap_settings_.enabled || !start_bounds.isValid()) {
        clear_snap_feedback();
        return delta;
    }

    snap_feedback_.clear();
    QPointF snapped_delta = delta;
    const double tolerance = 6.0 / std::max(0.1, view_scale());

    auto snap_axis = [&](bool x_axis) {
        std::vector<double> targets;
        std::vector<QString> labels;
        collect_snap_targets(x_axis, targets, labels);
        collect_spacing_targets(x_axis, targets, labels);
        if (targets.empty()) return;

        const double offset = x_axis ? snapped_delta.x() : snapped_delta.y();
        const double start_min = x_axis ? start_bounds.left() : start_bounds.top();
        const double start_center = x_axis ? start_bounds.center().x() : start_bounds.center().y();
        const double start_max = x_axis ? start_bounds.right() : start_bounds.bottom();
        const double points[] = {start_min + offset, start_center + offset, start_max + offset};
        double best_adjust = 0.0;
        double best_distance = tolerance + 1.0;
        double best_target = 0.0;
        QString best_label;
        for (size_t i = 0; i < targets.size(); ++i) {
            for (double point : points) {
                double adjust = targets[i] - point;
                double distance = std::abs(adjust);
                if (distance < best_distance) {
                    best_distance = distance;
                    best_adjust = adjust;
                    best_target = targets[i];
                    best_label = labels[i];
                }
            }
        }
        if (best_distance <= tolerance) {
            if (x_axis) snapped_delta.setX(snapped_delta.x() + best_adjust);
            else snapped_delta.setY(snapped_delta.y() + best_adjust);
            add_snap_feedback(x_axis, best_target, best_label);
        }
    };

    if (snap_x) snap_axis(true);
    if (snap_y) snap_axis(false);
    return snapped_delta;
}

QPointF CanvasPreview::snap_canvas_point(const QPointF &canvas_pt, bool snap_x, bool snap_y)
{
    QRectF point_bounds(canvas_pt, QSizeF(0.0, 0.0));
    QPointF delta = snap_delta_for_bounds(point_bounds, QPointF(0.0, 0.0), snap_x, snap_y);
    return canvas_pt + delta;
}

void CanvasPreview::apply_drag(const QPointF &view_pt, Qt::KeyboardModifiers modifiers)
{
    if (drag_mode_ == DragMode::Marquee) {
        update_marquee(view_pt, modifiers);
        return;
    }

    if (drag_mode_ == DragMode::Move && alt_duplicate_pending_ && !alt_duplicate_done_) {
        alt_duplicate_done_ = true;
        duplicate_selected_layers_for_drag();
    }

    auto layers = selected_layers();
    if (layers.empty() || drag_mode_ == DragMode::None) return;

    drag_current_view_ = view_pt;
    QPointF canvas = view_to_canvas(view_pt);
    QPointF delta = canvas - drag_start_canvas_;

    if (drag_mode_ == DragMode::Rotate) {
        clear_snap_feedback();
        QPointF pivot_view = canvas_to_view(drag_rotation_pivot_canvas_);
        double current_angle = radians_to_degrees(std::atan2(view_pt.y() - pivot_view.y(),
                                                             view_pt.x() - pivot_view.x()));
        double rotation_delta = normalize_degrees(current_angle - drag_start_rotation_angle_);
        if (modifiers & Qt::ShiftModifier)
            rotation_delta = std::round(rotation_delta / CANVAS_ROTATION_SNAP_DEGREES) * CANVAS_ROTATION_SNAP_DEGREES;
        drag_current_rotation_delta_ = rotation_delta;

        for (const auto &state : drag_layer_states_) {
            auto layer = title_->find_layer(state.id);
            if (!layer || layer->locked) continue;
            double lt = std::clamp(playhead_ - layer->in_time, 0.0,
                                   std::max(0.0, layer->out_time - layer->in_time));
            if (layers.size() > 1) {
                QPointF next_pos = rotate_point_around(QPointF(state.x, state.y), drag_rotation_pivot_canvas_, rotation_delta);
                set_animated_value(layer->pos_x, lt, next_pos.x());
                set_animated_value(layer->pos_y, lt, next_pos.y());
            }
            set_animated_value(layer->rotation, lt, state.rotation + rotation_delta);
        }
        dirty_ = true;
        drag_changed_ = true;
        update();
        return;
    }

    if (layers.size() > 1) {
        if (drag_mode_ == DragMode::Move) {
            if (modifiers & Qt::ShiftModifier) {
                if (std::abs(delta.x()) >= std::abs(delta.y())) delta.setY(0.0);
                else delta.setX(0.0);
            }
            delta = snap_delta_for_bounds(drag_start_selection_bounds_, delta, true, true);
            for (const auto &state : drag_layer_states_) {
                auto layer = title_->find_layer(state.id);
                if (!layer || layer->locked) continue;
                double lt = std::clamp(playhead_ - layer->in_time, 0.0,
                                       std::max(0.0, layer->out_time - layer->in_time));
                set_animated_value(layer->pos_x, lt, state.x + delta.x());
                set_animated_value(layer->pos_y, lt, state.y + delta.y());
            }
        } else {
            QRectF start = drag_start_selection_bounds_;
            if (!start.isValid() || start.width() <= 0.0 || start.height() <= 0.0) return;
            QRectF next = start;
            bool resize_left = drag_mode_ == DragMode::ResizeNW || drag_mode_ == DragMode::ResizeSW || drag_mode_ == DragMode::ResizeW;
            bool resize_right = drag_mode_ == DragMode::ResizeNE || drag_mode_ == DragMode::ResizeSE || drag_mode_ == DragMode::ResizeE;
            bool resize_top = drag_mode_ == DragMode::ResizeNW || drag_mode_ == DragMode::ResizeNE || drag_mode_ == DragMode::ResizeN;
            bool resize_bottom = drag_mode_ == DragMode::ResizeSW || drag_mode_ == DragMode::ResizeSE || drag_mode_ == DragMode::ResizeS;
            canvas = snap_canvas_point(canvas, resize_left || resize_right, resize_top || resize_bottom);
            if (resize_left) next.setLeft(std::min(canvas.x(), start.right()));
            if (resize_right) next.setRight(std::max(canvas.x(), start.left()));
            if (resize_top) next.setTop(std::min(canvas.y(), start.bottom()));
            if (resize_bottom) next.setBottom(std::max(canvas.y(), start.top()));
            double sx = next.width() / start.width();
            double sy = next.height() / start.height();
            if (modifiers & Qt::ShiftModifier) {
                double uniform = std::abs(sx) >= std::abs(sy) ? sx : sy;
                sx = sy = uniform;
            }
            for (const auto &state : drag_layer_states_) {
                auto layer = title_->find_layer(state.id);
                if (!layer || layer->locked) continue;
                double lt = std::clamp(playhead_ - layer->in_time, 0.0,
                                       std::max(0.0, layer->out_time - layer->in_time));
                double rx = (state.x - start.left()) / start.width();
                double ry = (state.y - start.top()) / start.height();
                set_animated_value(layer->pos_x, lt, next.left() + rx * next.width());
                set_animated_value(layer->pos_y, lt, next.top() + ry * next.height());
                const bool scale_text_object = drag_text_object_scaling_ && is_canvas_text_layer(*layer);
                if (scale_text_object) {
                    set_animated_value(layer->scale_x, lt, state.scale_x * sx);
                    set_animated_value(layer->scale_y, lt, state.scale_y * sy);
                } else {
                    layer->rect_width = std::max(0.0f, (float)(state.w * sx));
                    layer->rect_height = std::max(0.0f, (float)(state.h * sy));
                    set_animated_value(layer->box_width, lt, layer->rect_width);
                    set_animated_value(layer->box_height, lt, layer->rect_height);
                }
            }
        }
        dirty_ = true;
        drag_changed_ = true;
        update();
        return;
    }

    auto layer = layers.front();
    if (!layer) return;
    double lt = std::clamp(playhead_ - layer->in_time, 0.0,
                           std::max(0.0, layer->out_time - layer->in_time));

    if (drag_mode_ == DragMode::Move) {
        if (modifiers & Qt::ShiftModifier) {
            if (std::abs(delta.x()) >= std::abs(delta.y()))
                delta.setY(0.0);
            else
                delta.setX(0.0);
        }
        delta = snap_delta_for_bounds(drag_start_selection_bounds_, delta, true, true);
        set_animated_value(layer->pos_x, lt, drag_start_x_ + delta.x());
        set_animated_value(layer->pos_y, lt, drag_start_y_ + delta.y());
    } else if (drag_mode_ == DragMode::Origin) {
        clear_snap_feedback();
        double w = std::max(1.0f, drag_start_w_);
        double h = std::max(1.0f, drag_start_h_);
        layer->origin_x = (float)std::clamp(drag_start_origin_x_ + delta.x() / w, 0.0, 1.0);
        layer->origin_y = (float)std::clamp(drag_start_origin_y_ + delta.y() / h, 0.0, 1.0);
        set_animated_value(layer->origin_x_prop, lt, layer->origin_x);
        set_animated_value(layer->origin_y_prop, lt, layer->origin_y);
        set_animated_value(layer->pos_x, lt, drag_start_x_ + delta.x());
        set_animated_value(layer->pos_y, lt, drag_start_y_ + delta.y());
    } else {
        bool resize_left = drag_mode_ == DragMode::ResizeNW || drag_mode_ == DragMode::ResizeSW || drag_mode_ == DragMode::ResizeW;
        bool resize_right = drag_mode_ == DragMode::ResizeNE || drag_mode_ == DragMode::ResizeSE || drag_mode_ == DragMode::ResizeE;
        bool resize_top = drag_mode_ == DragMode::ResizeNW || drag_mode_ == DragMode::ResizeNE || drag_mode_ == DragMode::ResizeN;
        bool resize_bottom = drag_mode_ == DragMode::ResizeSW || drag_mode_ == DragMode::ResizeSE || drag_mode_ == DragMode::ResizeS;
        canvas = snap_canvas_point(canvas, resize_left || resize_right, resize_top || resize_bottom);
        const bool scale_text_object = drag_text_object_scaling_ && is_canvas_text_layer(*layer);
        const LayerDragState *start_state = drag_layer_states_.empty() ? nullptr : &drag_layer_states_.front();
        auto non_zero_scale = [](double value) {
            if (std::abs(value) >= 0.0001) return value;
            return value < 0.0 ? -0.0001 : 0.0001;
        };
        auto start_canvas_to_layer = [&](const QPointF &canvas_pt) {
            double rot = -start_state->rotation * 3.14159265358979323846 / 180.0;
            double dx = canvas_pt.x() - start_state->x;
            double dy = canvas_pt.y() - start_state->y;
            double c = std::cos(rot);
            double ss = std::sin(rot);
            return QPointF((dx * c - dy * ss) / non_zero_scale(start_state->scale_x),
                           (dx * ss + dy * c) / non_zero_scale(start_state->scale_y));
        };
        QPointF local = (scale_text_object && start_state) ? start_canvas_to_layer(canvas)
                                                           : canvas_to_layer(*layer, canvas);
        double left = -drag_start_origin_x_ * drag_start_w_;
        double right = (1.0 - drag_start_origin_x_) * drag_start_w_;
        double top = -drag_start_origin_y_ * drag_start_h_;
        double bottom = (1.0 - drag_start_origin_y_) * drag_start_h_;

        if (resize_left) left = std::min(local.x(), right);
        else if (resize_right) right = std::max(local.x(), left);
        if (resize_top) top = std::min(local.y(), bottom);
        else if (resize_bottom) bottom = std::max(local.y(), top);

        double new_w = std::max(0.0, right - left);
        double new_h = std::max(0.0, bottom - top);
        if (scale_text_object) {
            double sx = drag_start_w_ > 0.0f ? new_w / drag_start_w_ : 1.0;
            double sy = drag_start_h_ > 0.0f ? new_h / drag_start_h_ : 1.0;
            if (modifiers & Qt::ShiftModifier) {
                double uniform = std::abs(sx) >= std::abs(sy) ? sx : sy;
                sx = sy = uniform;
            }
            double start_scale_x = start_state ? start_state->scale_x : layer->scale_x.evaluate(lt);
            double start_scale_y = start_state ? start_state->scale_y : layer->scale_y.evaluate(lt);
            set_animated_value(layer->scale_x, lt, start_scale_x * sx);
            set_animated_value(layer->scale_y, lt, start_scale_y * sy);
        } else {
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
        if (box.width() <= 0.0 || box.height() <= 0.0) {
            p.restore();
            continue;
        }

        if (layer->type == LayerType::SolidRect || layer->type == LayerType::Shape) {
            QColor fc = color_from_argb(eval_fill_color(*layer, lt));
            if (eval_shadow_enabled(*layer, lt)) {
                QColor sc = color_from_argb(eval_shadow_color(*layer, lt));
                sc.setAlphaF(std::clamp((double)sc.alphaF() * eval_shadow_opacity(*layer, lt), 0.0, 1.0));
                QPointF off = shadow_offset(*layer, lt);
                double blur = eval_shadow_blur(*layer, lt);
                double spread = eval_shadow_spread(*layer, lt);
                int passes = shadow_pass_count(blur);
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
            double outline_width = eval_outline_width(*layer, lt);
            QColor outline = color_from_argb(eval_outline_color(*layer, lt));
            outline.setAlphaF(std::clamp((double)outline.alphaF() * eval_outline_opacity(*layer, lt), 0.0, 1.0));
            auto draw_shape = [&](const QBrush &brush, const QPen &pen) {
                p.setBrush(brush);
                p.setPen(pen);
                if (layer->corner_radius > 0)
                    p.drawRoundedRect(box, layer->corner_radius, layer->corner_radius);
                else
                    p.drawRect(box);
            };
            auto draw_outline = [&]() {
                if (outline_width <= 0.0 || outline.alpha() <= 0) return;
                bool previous_aa = p.testRenderHint(QPainter::Antialiasing);
                p.setRenderHint(QPainter::Antialiasing, eval_outline_antialias(*layer, lt));
                draw_shape(QBrush(Qt::NoBrush), QPen(outline, outline_width, Qt::SolidLine, Qt::SquareCap, outline_pen_join_style(*layer)));
                p.setRenderHint(QPainter::Antialiasing, previous_aa);
            };
            if (!eval_outline_on_front(*layer, lt)) draw_outline();
            const QBrush fill_brush = layer->fill_type == 1 ? gradient_fill_brush(*layer, box) : QBrush(fc);
            draw_shape(fill_brush, QPen(Qt::NoPen));
            if (eval_outline_on_front(*layer, lt)) draw_outline();
        }

        if (layer->type == LayerType::Image) {
            if (eval_background_enabled(*layer, lt)) {
                QColor bg = evaluated_background_color(*layer, lt);
                if (bg.alpha() > 0 || layer->fill_type == 1) {
                    const double pad_x = eval_background_padding_x(*layer, lt);
                    const double pad_y = eval_background_padding_y(*layer, lt);
                    const double corner = eval_background_corner_radius(*layer, lt);
                    QRectF bg_box = box.adjusted(-pad_x, -pad_y, pad_x, pad_y);
                    p.setPen(Qt::NoPen);
                    p.setBrush(layer->fill_type == 1 ? gradient_fill_brush(*layer, bg_box, eval_background_opacity(*layer, lt)) : QBrush(bg));
                    p.drawRoundedRect(bg_box, corner, corner);
                }
            }
            QImage image = editor_load_layer_image(QString::fromStdString(layer->image_path),
                                                   box.size().toSize());
            if (!image.isNull()) {
                p.drawImage(box, image);
            } else {
                p.setBrush(QColor(0x33, 0x33, 0x33));
                p.setPen(QPen(QColor(0xff, 0x55, 0x55), 2));
                p.drawRect(box);
                p.drawText(box, Qt::AlignCenter, obsgs_tr("OBSTitles.MissingImage"));
            }
        }

        if (layer->type == LayerType::Text || layer->type == LayerType::Clock || layer->type == LayerType::Ticker) {
            QColor tc = color_from_argb(eval_text_color(*layer, lt));
            QFont f = font_for_layer(*layer);
            p.setFont(f);
            QString text = display_text_for_style(*layer);
            if (eval_background_enabled(*layer, lt)) {
                QColor bg = evaluated_background_color(*layer, lt);
                if (bg.alpha() > 0 || layer->fill_type == 1) {
                    const double pad_x = eval_background_padding_x(*layer, lt);
                    const double pad_y = eval_background_padding_y(*layer, lt);
                    const double corner = eval_background_corner_radius(*layer, lt);
                    QRectF bg_box = box.adjusted(-pad_x, -pad_y, pad_x, pad_y);
                    p.setPen(Qt::NoPen);
                    p.setBrush(layer->fill_type == 1 ? gradient_fill_brush(*layer, bg_box, eval_background_opacity(*layer, lt)) : QBrush(bg));
                    p.drawRoundedRect(bg_box, corner, corner);
                }
            }
            QRectF text_box = text_rect_for_style(box, *layer);
            p.save();
            p.setClipRect(text_box);
            Qt::AlignmentFlag ha = Qt::AlignHCenter;
            if (layer->align_h == 0) ha = Qt::AlignLeft;
            if (layer->align_h == 2) ha = Qt::AlignRight;
            Qt::AlignmentFlag va = Qt::AlignVCenter;
            if (layer->align_v == 0) va = Qt::AlignTop;
            if (layer->align_v == 2) va = Qt::AlignBottom;
            QPainterPath text_path = layer->type == LayerType::Ticker
                ? ticker_text_path(f, text_box, ha | va, text, *layer)
                : text_overflow_path(f, text_box, ha | va, text, *layer);
            text_path = apply_vertical_character_scale(text_path, text_box, ha | va, *layer);
            if (eval_shadow_enabled(*layer, lt)) {
                QColor sc = color_from_argb(eval_shadow_color(*layer, lt));
                sc.setAlphaF(std::clamp((double)sc.alphaF() * eval_shadow_opacity(*layer, lt), 0.0, 1.0));
                QPointF off = shadow_offset(*layer, lt);
                double blur = eval_shadow_blur(*layer, lt);
                double spread = eval_shadow_spread(*layer, lt);
                int passes = shadow_pass_count(blur);
                for (int pass = passes; pass >= 1; --pass) {
                    QColor pass_color = sc;
                    pass_color.setAlphaF(sc.alphaF() / passes);
                    p.setPen(Qt::NoPen);
                    p.setBrush(pass_color);
                    double radius = blur * pass / passes;
                    for (double dx : {-spread - radius, 0.0, spread + radius})
                        for (double dy : {-spread - radius, 0.0, spread + radius})
                            p.drawPath(text_path.translated(off + QPointF(dx, dy)));
                }
            }
            double outline_width = eval_outline_width(*layer, lt);
            QColor outline = color_from_argb(eval_outline_color(*layer, lt));
            outline.setAlphaF(std::clamp((double)outline.alphaF() * eval_outline_opacity(*layer, lt), 0.0, 1.0));
            auto draw_text_fill = [&]() {
                p.setPen(Qt::NoPen);
                p.setBrush(layer->fill_type == 1 ? gradient_fill_brush(*layer, text_box) : QBrush(tc));
                p.drawPath(text_path);
            };
            auto draw_text_outline = [&]() {
                if (outline_width <= 0.0 || outline.alpha() <= 0) return;
                bool previous_aa = p.testRenderHint(QPainter::Antialiasing);
                p.setRenderHint(QPainter::Antialiasing, eval_outline_antialias(*layer, lt));
                p.setPen(QPen(outline, outline_width, Qt::SolidLine, Qt::RoundCap, outline_pen_join_style(*layer)));
                p.setBrush(Qt::NoBrush);
                p.drawPath(text_path);
                p.setRenderHint(QPainter::Antialiasing, previous_aa);
            };
            if (!eval_outline_on_front(*layer, lt)) draw_text_outline();
            draw_text_fill();
            if (eval_outline_on_front(*layer, lt)) draw_text_outline();
            p.restore();
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

    if (title_has_dynamic_text_layer(title_)) dirty_ = true;
    if (dirty_) render_to_pixmap();
    if (frame_pixmap_.isNull()) return;

    double scale = view_scale();
    QPointF origin = view_origin();
    int dw = (int)(title_->width * scale);
    int dh = (int)(title_->height * scale);
    int ox = (int)origin.x();
    int oy = (int)origin.y();

    auto checkerboard_colors = [this]() {
        if (checkerboard_pattern_ == 0)
            return std::pair<QColor, QColor>(QColor(0xee, 0xee, 0xee), QColor(0xc8, 0xc8, 0xc8));
        if (checkerboard_pattern_ == 2)
            return std::pair<QColor, QColor>(QColor(0x1f, 0x1f, 0x1f), QColor(0x36, 0x36, 0x36));
        return std::pair<QColor, QColor>(QColor(0x33, 0x33, 0x33), QColor(0x4a, 0x4a, 0x4a));
    };
    const auto [checker_a, checker_b] = checkerboard_colors();
    p.setPen(Qt::NoPen);
    p.setBrush(QBrush(checker_a));
    p.drawRect(ox, oy, dw, dh);
    p.setBrush(QBrush(checker_b));
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
        draw_guide(OBS_ACTION_SAFE_PERCENT, QColor(0, 200, 255, 190));
        draw_guide(OBS_GRAPHICS_SAFE_PERCENT, QColor(255, 220, 0, 190));
    }

    if (!snap_feedback_.empty()) {
        p.save();
        p.setRenderHint(QPainter::Antialiasing, false);
        QPen guide_pen(QColor(0, 220, 255, 235), 1.0, Qt::DashLine);
        guide_pen.setDashPattern({5.0, 3.0});
        p.setPen(guide_pen);
        p.setBrush(QColor(0, 20, 30, 180));
        for (const auto &feedback : snap_feedback_) {
            if (feedback.x_axis) {
                double x = canvas_to_view(QPointF(feedback.value, 0.0)).x();
                p.drawLine(QPointF(x, oy), QPointF(x, oy + dh));
                if (!feedback.label.isEmpty())
                    p.drawText(QRectF(x + 5.0, oy + 5.0, 120.0, 18.0), feedback.label);
            } else {
                double y = canvas_to_view(QPointF(0.0, feedback.value)).y();
                p.drawLine(QPointF(ox, y), QPointF(ox + dw, y));
                if (!feedback.label.isEmpty())
                    p.drawText(QRectF(ox + 5.0, y + 5.0, 120.0, 18.0), feedback.label);
            }
        }
        p.restore();
    }

    auto layers = selected_layers();

    auto draw_layer_box = [&](const Layer &layer, bool handles) {
        QRectF box = layer_local_rect(layer);
        auto layer_point_to_view = [&](const QPointF &pt) {
            return canvas_to_view(layer_to_canvas(layer, pt));
        };
        const QPointF corners[] = {
            layer_point_to_view(box.topLeft()),
            layer_point_to_view(box.topRight()),
            layer_point_to_view(box.bottomRight()),
            layer_point_to_view(box.bottomLeft())
        };

        p.save();
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(QColor(0, 120, 255, handles ? 230 : 150), 1.5, Qt::DashLine));
        QPolygonF outline;
        for (const QPointF &corner : corners)
            outline << corner;
        p.drawPolygon(outline);
        if (handles) {
            p.setPen(QPen(QColor(0, 120, 255, 255), 1.0));
            p.setBrush(QColor(255, 255, 255));
            const QPointF handle_points[] = {
                corners[0], layer_point_to_view(QPointF(box.center().x(), box.top())), corners[1],
                layer_point_to_view(QPointF(box.right(), box.center().y())), corners[2],
                layer_point_to_view(QPointF(box.center().x(), box.bottom())), corners[3],
                layer_point_to_view(QPointF(box.left(), box.center().y()))
            };
            const double half_handle = CANVAS_CONTROL_SIZE_PX / 2.0;
            for (const QPointF &pt : handle_points)
                p.drawRect(QRectF(pt.x() - half_handle, pt.y() - half_handle,
                                  CANVAS_CONTROL_SIZE_PX, CANVAS_CONTROL_SIZE_PX));

            QPointF origin = layer_point_to_view(QPointF(0, 0));
            p.setPen(QPen(QColor(255, 160, 0), 1.5));
            p.setBrush(QColor(255, 220, 80));
            p.drawEllipse(origin, CANVAS_ORIGIN_RADIUS_PX, CANVAS_ORIGIN_RADIUS_PX);
            p.drawLine(QPointF(origin.x() - CANVAS_CONTROL_SIZE_PX, origin.y()),
                       QPointF(origin.x() + CANVAS_CONTROL_SIZE_PX, origin.y()));
            p.drawLine(QPointF(origin.x(), origin.y() - CANVAS_CONTROL_SIZE_PX),
                       QPointF(origin.x(), origin.y() + CANVAS_CONTROL_SIZE_PX));
        }
        p.restore();
    };

    if (layers.size() == 1) {
        draw_layer_box(*layers.front(), true);
    } else if (layers.size() > 1) {
        for (const auto &layer : layers)
            if (layer) draw_layer_box(*layer, false);

        QRectF bounds = selected_canvas_bounds();
        if (bounds.isValid() && !bounds.isEmpty()) {
            QRectF view_bounds(canvas_to_view(bounds.topLeft()), canvas_to_view(bounds.bottomRight()));
            view_bounds = view_bounds.normalized();
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(QColor(0, 160, 255, 240), 1.5, Qt::SolidLine));
            p.drawRect(view_bounds);
            p.setPen(QPen(QColor(0, 120, 255, 255), 1.0));
            p.setBrush(QColor(255, 255, 255));
            const QPointF points[] = {
                view_bounds.topLeft(), QPointF(view_bounds.center().x(), view_bounds.top()), view_bounds.topRight(),
                QPointF(view_bounds.right(), view_bounds.center().y()), view_bounds.bottomRight(),
                QPointF(view_bounds.center().x(), view_bounds.bottom()), view_bounds.bottomLeft(),
                QPointF(view_bounds.left(), view_bounds.center().y())
            };
            for (const QPointF &pt : points)
                p.drawRect(QRectF(pt.x() - CANVAS_CONTROL_SIZE_PX / 2.0,
                                  pt.y() - CANVAS_CONTROL_SIZE_PX / 2.0,
                                  CANVAS_CONTROL_SIZE_PX, CANVAS_CONTROL_SIZE_PX));
        }
    }

    if (drag_mode_ == DragMode::Rotate) {
        QPointF pivot = canvas_to_view(drag_rotation_pivot_canvas_);
        double start_angle = std::atan2(drag_start_view_.y() - pivot.y(), drag_start_view_.x() - pivot.x());
        double radius = std::max(24.0, QLineF(pivot, drag_current_view_).length());
        QRectF arc_rect(pivot.x() - radius, pivot.y() - radius, radius * 2.0, radius * 2.0);
        p.save();
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(QColor(255, 190, 40, 235), 1.5, Qt::DashLine));
        p.drawLine(pivot, drag_current_view_);
        p.setPen(QPen(QColor(255, 190, 40, 235), 2.0));
        p.drawEllipse(pivot, CANVAS_ORIGIN_RADIUS_PX + 2.0, CANVAS_ORIGIN_RADIUS_PX + 2.0);
        p.drawArc(arc_rect, (int)std::round(-radians_to_degrees(start_angle) * 16.0),
                  (int)std::round(-drag_current_rotation_delta_ * 16.0));
        QString label = QStringLiteral("%1°").arg(drag_current_rotation_delta_, 0, 'f', 1);
        QRectF label_rect(drag_current_view_.x() + 12.0, drag_current_view_.y() + 12.0, 72.0, 22.0);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(20, 20, 20, 210));
        p.drawRoundedRect(label_rect, 4.0, 4.0);
        p.setPen(QColor(255, 230, 150));
        p.drawText(label_rect, Qt::AlignCenter, label);
        p.restore();
    }

    if (drag_mode_ == DragMode::Marquee && marquee_active_) {
        QRectF marquee(drag_start_view_, drag_current_view_);
        marquee = marquee.normalized();
        p.setBrush(QColor(0, 120, 255, 45));
        p.setPen(QPen(QColor(0, 160, 255, 220), 1.0, Qt::DashLine));
        p.drawRect(marquee);
    }
}
void CanvasPreview::mousePressEvent(QMouseEvent *ev)
{
    setFocus(Qt::MouseFocusReason);
    if (!title_) return;

    if (ev->button() == Qt::MiddleButton) {
        panning_ = true;
        pan_start_view_ = QPointF(ev->pos());
        pan_start_offset_ = pan_offset_;
        setCursor(Qt::ClosedHandCursor);
        ev->accept();
        return;
    }

    if (ev->button() != Qt::LeftButton) return;

    drag_mode_ = hit_test_selected(ev->pos());
    if (drag_mode_ == DragMode::None) {
        QPointF canvas = view_to_canvas(ev->pos());
        for (auto it = title_->layers.rbegin(); it != title_->layers.rend(); ++it) {
            auto &l = *it;
            if (!l || !l->visible || l->locked) continue;
            if (playhead_ < l->in_time || playhead_ > l->out_time) continue;
            QPointF local = canvas_to_layer(*l, canvas);
            if (layer_local_rect(*l).contains(local)) {
                std::vector<std::string> next_ids;
                if (ev->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier))
                    next_ids = selected_layer_ids_;
                auto existing = std::find(next_ids.begin(), next_ids.end(), l->id);
                if (ev->modifiers() & Qt::ControlModifier) {
                    if (existing == next_ids.end()) next_ids.push_back(l->id);
                    else next_ids.erase(existing);
                } else if (existing == next_ids.end()) {
                    if (!(ev->modifiers() & Qt::ShiftModifier)) next_ids.clear();
                    next_ids.push_back(l->id);
                }
                emit layers_selected(next_ids);
                selected_layer_ids_ = next_ids;
                sel_layer_id_ = selected_layer_ids_.empty() ? std::string() : selected_layer_ids_.back();
                drag_mode_ = DragMode::Move;
                break;
            }
        }
    }

    if (drag_mode_ == DragMode::None) {
        begin_marquee(ev->pos(), ev->modifiers());
        ev->accept();
        return;
    }

    drag_changed_ = false;
    alt_duplicate_pending_ = (drag_mode_ == DragMode::Move) && ev->modifiers().testFlag(Qt::AltModifier);
    alt_duplicate_done_ = false;
    drag_start_view_ = ev->pos();
    drag_current_view_ = ev->pos();
    drag_start_canvas_ = view_to_canvas(ev->pos());
    drag_layer_states_.clear();
    drag_start_selection_bounds_ = selected_canvas_bounds();

    auto layers = selected_layers();
    drag_text_object_scaling_ = false;
    auto layer = selected_layer();
    if (!layer && !layers.empty()) layer = layers.front();
    if (!layer) return;

    auto is_resize_drag = [](DragMode mode) {
        return mode == DragMode::ResizeNW || mode == DragMode::ResizeN || mode == DragMode::ResizeNE ||
               mode == DragMode::ResizeE || mode == DragMode::ResizeSE || mode == DragMode::ResizeS ||
               mode == DragMode::ResizeSW || mode == DragMode::ResizeW;
    };
    drag_text_object_scaling_ = is_resize_drag(drag_mode_) && ev->modifiers().testFlag(Qt::AltModifier) &&
        std::any_of(layers.begin(), layers.end(), [](const std::shared_ptr<Layer> &selected) {
            return selected && is_canvas_text_layer(*selected);
        });

    for (const auto &selected : layers) {
        if (!selected || selected->locked) continue;
        double lt = std::clamp(playhead_ - selected->in_time, 0.0,
                               std::max(0.0, selected->out_time - selected->in_time));
        drag_layer_states_.push_back({selected->id,
                                      selected->pos_x.evaluate(lt),
                                      selected->pos_y.evaluate(lt),
                                      (float)eval_box_width(*selected, lt),
                                      (float)eval_box_height(*selected, lt),
                                      selected->scale_x.evaluate(lt),
                                      selected->scale_y.evaluate(lt),
                                      selected->rotation.evaluate(lt)});
    }

    double lt = std::clamp(playhead_ - layer->in_time, 0.0,
                           std::max(0.0, layer->out_time - layer->in_time));
    drag_start_x_ = layer->pos_x.evaluate(lt);
    drag_start_y_ = layer->pos_y.evaluate(lt);
    drag_start_w_ = (float)eval_box_width(*layer, lt);
    drag_start_h_ = (float)eval_box_height(*layer, lt);
    drag_start_origin_x_ = layer->origin_x;
    drag_start_origin_y_ = layer->origin_y;
    drag_rotation_pivot_canvas_ = layers.size() > 1
        ? drag_start_selection_bounds_.center()
        : layer_to_canvas(*layer, QPointF(0, 0));
    QPointF pivot_view = canvas_to_view(drag_rotation_pivot_canvas_);
    drag_start_rotation_angle_ = radians_to_degrees(std::atan2(drag_start_view_.y() - pivot_view.y(),
                                                               drag_start_view_.x() - pivot_view.x()));
    drag_current_rotation_delta_ = 0.0;
    auto set_cursor_for_mode = [this](DragMode mode) {
        if (mode == DragMode::Move) setCursor(Qt::ClosedHandCursor);
        else if (mode == DragMode::Origin) setCursor(Qt::CrossCursor);
        else if (mode == DragMode::Rotate) setCursor(canvas_rotation_cursor());
        else if (mode == DragMode::ResizeN || mode == DragMode::ResizeS) setCursor(Qt::SizeVerCursor);
        else if (mode == DragMode::ResizeE || mode == DragMode::ResizeW) setCursor(Qt::SizeHorCursor);
        else if (mode == DragMode::ResizeNE || mode == DragMode::ResizeSW) setCursor(Qt::SizeBDiagCursor);
        else setCursor(Qt::SizeFDiagCursor);
    };
    set_cursor_for_mode(drag_mode_);
    ev->accept();
}

void CanvasPreview::mouseMoveEvent(QMouseEvent *ev)
{
    if (panning_ && (ev->buttons() & Qt::MiddleButton)) {
        pan_offset_ = pan_start_offset_ + (QPointF(ev->pos()) - pan_start_view_);
        fit_zoom_active_ = false;
        update();
        ev->accept();
        return;
    }

    if (drag_mode_ != DragMode::None && (ev->buttons() & Qt::LeftButton)) {
        apply_drag(ev->pos(), ev->modifiers());
        ev->accept();
        return;
    }

    DragMode mode = hit_test_selected(ev->pos());
    if (mode == DragMode::Move) setCursor(Qt::OpenHandCursor);
    else if (mode == DragMode::Origin) setCursor(Qt::CrossCursor);
    else if (mode == DragMode::Rotate) setCursor(canvas_rotation_cursor());
    else if (mode == DragMode::ResizeN || mode == DragMode::ResizeS) setCursor(Qt::SizeVerCursor);
    else if (mode == DragMode::ResizeE || mode == DragMode::ResizeW) setCursor(Qt::SizeHorCursor);
    else if (mode == DragMode::ResizeNE || mode == DragMode::ResizeSW) setCursor(Qt::SizeBDiagCursor);
    else if (mode != DragMode::None) setCursor(Qt::SizeFDiagCursor);
    else unsetCursor();
}

void CanvasPreview::keyPressEvent(QKeyEvent *ev)
{
    double dx = 0.0;
    double dy = 0.0;
    const double amount = ev->modifiers().testFlag(Qt::ShiftModifier) ? 10.0 : 1.0;

    switch (ev->key()) {
    case Qt::Key_Left:
        dx = -amount;
        break;
    case Qt::Key_Right:
        dx = amount;
        break;
    case Qt::Key_Up:
        dy = -amount;
        break;
    case Qt::Key_Down:
        dy = amount;
        break;
    default:
        QWidget::keyPressEvent(ev);
        return;
    }

    if (nudge_selected_layers(dx, dy))
        ev->accept();
    else
        QWidget::keyPressEvent(ev);
}

void CanvasPreview::mouseReleaseEvent(QMouseEvent *ev)
{
    if (ev->button() == Qt::MiddleButton && panning_) {
        panning_ = false;
        unsetCursor();
        ev->accept();
        return;
    }

    if (ev->button() != Qt::LeftButton || drag_mode_ == DragMode::None) return;

    if (drag_mode_ == DragMode::Marquee) {
        update_marquee(ev->pos(), ev->modifiers());
        if (!marquee_active_)
            emit layers_selected(std::vector<std::string>{});
        drag_mode_ = DragMode::None;
        marquee_active_ = false;
        drag_changed_ = false;
        alt_duplicate_pending_ = false;
        alt_duplicate_done_ = false;
        drag_text_object_scaling_ = false;
        clear_snap_feedback();
        unsetCursor();
        update();
        ev->accept();
        return;
    }

    bool changed = drag_changed_;
    drag_mode_ = DragMode::None;
    drag_changed_ = false;
    alt_duplicate_pending_ = false;
    alt_duplicate_done_ = false;
    drag_text_object_scaling_ = false;
    drag_layer_states_.clear();
    clear_snap_feedback();
    unsetCursor();
    if (changed)
        emit layer_geometry_changed();
    ev->accept();
}


void CanvasPreview::wheelEvent(QWheelEvent *ev)
{
    if (!title_) return;
    QPointF anchor_canvas = view_to_canvas(ev->position());
    int next = zoom_percent_;
    if (ev->angleDelta().y() > 0)
        next = (int)std::round(next * 1.1);
    else
        next = (int)std::round(next / 1.1);
    zoom_percent_ = std::clamp(next, 5, 1600);
    fit_zoom_active_ = false;
    QPointF origin_without_pan = centered_view_origin();
    double scale = view_scale();
    pan_offset_ = ev->position() - origin_without_pan - QPointF(anchor_canvas.x() * scale, anchor_canvas.y() * scale);
    emit zoom_percent_changed(zoom_percent_);
    update();
    ev->accept();
}

void CanvasPreview::resizeEvent(QResizeEvent *)
{
    dirty_ = true;
    if (fit_zoom_active_) fit_canvas(fit_zoom_up_to_100_);
}

/* ══════════════════════════════════════════════════════════════════
 *  LayerStack
 * ══════════════════════════════════════════════════════════════════ */
LayerStack::LayerStack(QWidget *parent) : QWidget(parent)
{
    setStyleSheet("background:#1a1a1a;");
    auto *vl = new QVBoxLayout(this);
    vl->setContentsMargins(0, 0, 0, 0);
    vl->setSpacing(0);


    QWidget *columns = new QWidget(this);
    columns->setFixedHeight(72);
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
    QLabel *name = new QLabel(obsgs_tr("OBSTitles.LayerNameHeader"), columns);
    name->setStyleSheet("color:#9a9a9a;font-size:10px;font-weight:bold;");
    ch->addWidget(name, 1);
    add_header(obsgs_tr("OBSTitles.ModeHeader"), 46, Qt::AlignLeft | Qt::AlignVCenter);
    add_header(obsgs_tr("OBSTitles.ParentHeader"), 58, Qt::AlignLeft | Qt::AlignVCenter);
    vl->addWidget(columns);

    list_ = new QListWidget(this);
    list_->setDragDropMode(QAbstractItemView::InternalMove);
    list_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    list_->setAlternatingRowColors(false);
    list_->setUniformItemSizes(false);
    list_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    list_->setStyleSheet(
        "QListWidget{background:#1a1a1a;border:none;color:#ccc;}"
        "QListWidget::item{border-bottom:1px solid #2a2a2a;}"
        "QListWidget::item:selected{background:#3b4f64;}"
        "QListWidget::item:hover{background:#252525;}");
    vl->addWidget(list_, 1);

    auto *toolbar = new QToolBar(this);
    toolbar->setMovable(false);
    toolbar->setFloatable(false);
    toolbar->setOrientation(Qt::Horizontal);
    toolbar->setToolButtonStyle(Qt::ToolButtonIconOnly);
    toolbar->setIconSize(QSize(16, 16));
    toolbar->setContentsMargins(0, 0, 0, 0);
    toolbar->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    auto make_layer_tool = [&](const QString &text, const QIcon &icon, const QString &tip) {
        auto *button = new QToolButton(toolbar);
        button->setText(text);
        button->setAccessibleName(text);
        button->setToolTip(tip);
        button->setIcon(icon);
        button->setIconSize(QSize(16, 16));
        button->setToolButtonStyle(Qt::ToolButtonIconOnly);
        button->setAutoRaise(true);
        button->setFocusPolicy(Qt::StrongFocus);
        return button;
    };

    btn_add_ = make_layer_tool(obsgs_tr("OBSTitles.AddLayer"),
                               obs_icon("add.svg"),
                               obsgs_tr("OBSTitles.AddLayerTooltip"));
    auto *add_menu = new QMenu(btn_add_);
    add_menu->addAction(obs_icon("text.svg"),
                        obsgs_tr("OBSTitles.Text"), this, &LayerStack::on_add_text);
    add_menu->addAction(obs_icon("clock.svg"),
                        obsgs_tr("OBSTitles.Clock"), this, &LayerStack::on_add_clock);
    add_menu->addAction(obs_icon("text.svg"),
                        obsgs_tr("OBSTitles.Ticker"), this, &LayerStack::on_add_ticker);
    add_menu->addAction(obs_icon("shape.svg"),
                        obsgs_tr("OBSTitles.Shape"), this, &LayerStack::on_add_rect);
    add_menu->addAction(obs_icon("image.svg"),
                        obsgs_tr("OBSTitles.Image"), this, &LayerStack::on_add_image);
    btn_add_->setMenu(add_menu);
    btn_add_->setPopupMode(QToolButton::InstantPopup);
    btn_add_->setStyleSheet(QStringLiteral("QToolButton::menu-indicator{image:none;width:0px;}"));

    btn_move_up_ = make_layer_tool(obsgs_tr("OBSTitles.MoveLayerUp"),
                                   obs_icon("move-up.svg"),
                                   obsgs_tr("OBSTitles.MoveLayerUpTooltip"));
    btn_move_down_ = make_layer_tool(obsgs_tr("OBSTitles.MoveLayerDown"),
                                     obs_icon("move-down.svg"),
                                     obsgs_tr("OBSTitles.MoveLayerDownTooltip"));
    btn_del_ = make_layer_tool(obsgs_tr("OBSTitles.DeleteLayer"),
                               obs_icon("delete.svg"),
                               obsgs_tr("OBSTitles.DeleteLayerTooltip"));
    btn_move_up_->setEnabled(false);
    btn_move_down_->setEnabled(false);
    btn_del_->setEnabled(false);

    toolbar->addWidget(btn_add_);
    toolbar->addWidget(btn_move_up_);
    toolbar->addWidget(btn_move_down_);
    toolbar->addSeparator();
    toolbar->addWidget(btn_del_);
    vl->addWidget(toolbar);

    connect(btn_move_up_, &QToolButton::clicked, this, &LayerStack::on_move_up);
    connect(btn_move_down_, &QToolButton::clicked, this, &LayerStack::on_move_down);
    connect(btn_del_, &QToolButton::clicked, this, &LayerStack::on_delete);
    connect(list_, &QListWidget::itemSelectionChanged,
            this, &LayerStack::on_selection_changed);
    connect(list_->model(), &QAbstractItemModel::rowsMoved,
            this, [this]() { sync_order_from_list(); });
    list_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(list_, &QListWidget::customContextMenuRequested,
            this, &LayerStack::show_layer_context_menu);
    list_->viewport()->installEventFilter(this);
}

bool LayerStack::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == list_->viewport() && event->type() == QEvent::MouseButtonPress) {
        auto *mouse_event = static_cast<QMouseEvent *>(event);
        if (mouse_event->button() == Qt::LeftButton && !list_->itemAt(mouse_event->pos())) {
            QSignalBlocker blocker(list_);
            list_->clearSelection();
            list_->setCurrentItem(nullptr);
            emit layer_selected(std::string());
            event->accept();
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void LayerStack::set_title(std::shared_ptr<Title> t)
{
    title_ = t; populate();
}

void LayerStack::refresh() { populate(); }

void LayerStack::set_layer_clipboard_available(bool available)
{
    layer_clipboard_available_ = available;
}

QScrollBar *LayerStack::vertical_scroll_bar() const
{
    return list_ ? list_->verticalScrollBar() : nullptr;
}

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
        item->setSizeHint(QSize(0, 28));
        list_->addItem(item);

        QWidget *row_widget = new QWidget(list_);
        row_widget->setStyleSheet("background:transparent;color:#d0d0d0;");
        auto *hl = new QHBoxLayout(row_widget);
        hl->setContentsMargins(4, 0, 4, 0);
        hl->setSpacing(4);

        auto make_toggle = [&](const char *on_icon, const char *off_icon, bool checked,
                               const QString &tip) {
            auto *btn = new QToolButton(row_widget);
            btn->setCheckable(true);
            btn->setChecked(checked);
            btn->setIcon(obs_icon(checked ? on_icon : off_icon));
            btn->setToolTip(tip);
            btn->setFixedSize(20, 20);
            btn->setIconSize(QSize(14, 14));
            btn->setAutoRaise(true);
            btn->setStyleSheet("QToolButton{color:#bcbcbc;background:transparent;border:none;}"
                               "QToolButton:hover{background:#353535;border-radius:2px;}"
                               "QToolButton:checked{color:#eeeeee;}");
            connect(btn, &QToolButton::toggled, btn, [btn, on_icon, off_icon](bool state) {
                btn->setIcon(obs_icon(state ? on_icon : off_icon));
            });
            hl->addWidget(btn);
            return btn;
        };

        QToolButton *vis = make_toggle("layer-visible.svg", "layer-hidden.svg", l->visible, obsgs_tr("OBSTitles.LayerVisibilityTooltip"));
        QToolButton *lock = make_toggle("layer-lock.svg", "layer-unlock.svg", l->locked, obsgs_tr("OBSTitles.LockLayerTooltip"));
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
        expand->setIcon(obs_icon(l->properties_expanded ? "keyframes-expand.svg" : "keyframes-collapse.svg"));
        expand->setToolTip(obsgs_tr("OBSTitles.ShowKeyframedPropertiesTooltip"));
        expand->setFixedSize(16, 20);
        expand->setIconSize(QSize(12, 12));
        expand->setAutoRaise(true);
        expand->setStyleSheet("QToolButton{color:#aaa;background:transparent;border:none;}"
                              "QToolButton:hover{background:#353535;border-radius:2px;}");
        connect(expand, &QToolButton::toggled, this, [this, expand, id = l->id](bool checked) {
            expand->setIcon(obs_icon(checked ? "keyframes-expand.svg" : "keyframes-collapse.svg"));
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
        name->setToolTip(obsgs_tr("OBSTitles.RenameLayerTooltip"));
        name->setStyleSheet(l->locked
            ? "QLineEdit{color:#8f8f8f;background:transparent;border:none;}"
            : "QLineEdit{color:#d0d0d0;background:transparent;border:none;padding:1px;} QLineEdit:focus{background:#101010;border:1px solid #0078d4;border-radius:2px;}");
        connect(name, &QLineEdit::editingFinished, this, [this, id = l->id, name]() {
            emit layer_name_changed(id, name->text().trimmed().toStdString());
        });
        hl->addWidget(name, 1);

        QLabel *mode = new QLabel(obsgs_tr("OBSTitles.Normal"), row_widget);
        mode->setFixedWidth(54);
        mode->setStyleSheet("color:#b0b0b0;background:#101010;border-radius:3px;padding-left:4px;");
        hl->addWidget(mode);

        QComboBox *parent = new QComboBox(row_widget);
        parent->setFixedWidth(86);
        parent->setStyleSheet("QComboBox{color:#b0b0b0;background:#101010;border:none;border-radius:3px;padding-left:4px;}"
                              "QComboBox::drop-down{border:none;}");
        parent->addItem(obsgs_tr("OBSTitles.None"), "");
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
            prop_item->setSizeHint(QSize(0, 28));
            list_->addItem(prop_item);

            QWidget *prop_widget = new QWidget(list_);
            auto *ph = new QHBoxLayout(prop_widget);
            ph->setContentsMargins(64, 0, 4, 0);
            ph->setSpacing(4);
            QLabel *diamond_indicator = new QLabel("◇", prop_widget);
            diamond_indicator->setFixedWidth(18);
            diamond_indicator->setAlignment(Qt::AlignCenter);
            diamond_indicator->setStyleSheet(QString("color:%1;").arg(layer_color(*l, row).name()));
            ph->addWidget(diamond_indicator);
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
    set_selected_layers(layer_id.empty() ? std::vector<std::string>()
                                         : std::vector<std::string>{layer_id});
}

void LayerStack::set_selected_layers(const std::vector<std::string> &layer_ids)
{
    QSignalBlocker blocker(list_);
    list_->clearSelection();
    if (layer_ids.empty()) {
        list_->setCurrentItem(nullptr);
        return;
    }

    std::set<QString> ids;
    for (const auto &id : layer_ids)
        ids.insert(QString::fromStdString(id));

    QListWidgetItem *current = nullptr;
    QString primary = QString::fromStdString(layer_ids.back());
    for (int i = 0; i < list_->count(); ++i) {
        auto *item = list_->item(i);
        if (item->data(Qt::UserRole + 1).toString() != "layer") continue;
        QString id = item->data(Qt::UserRole).toString();
        if (ids.find(id) != ids.end()) {
            item->setSelected(true);
            if (id == primary) current = item;
        }
    }
    if (current) list_->setCurrentItem(current, QItemSelectionModel::NoUpdate);
}

std::string LayerStack::selected_id() const
{
    auto *item = list_->currentItem();
    if (item && item->isSelected())
        return item->data(Qt::UserRole).toString().toStdString();
    auto selected = list_->selectedItems();
    return selected.isEmpty() ? std::string() : selected.back()->data(Qt::UserRole).toString().toStdString();
}

std::vector<std::string> LayerStack::selected_ids() const
{
    std::vector<std::string> ids;
    for (auto *item : list_->selectedItems())
        ids.push_back(item->data(Qt::UserRole).toString().toStdString());
    return ids;
}

void LayerStack::on_selection_changed()
{
    std::string id = selected_id();
    const bool has_layer = !id.empty() && title_ && title_->find_layer(id);
    if (btn_del_) btn_del_->setEnabled(has_layer);

    bool can_move_up = false;
    bool can_move_down = false;
    if (has_layer) {
        auto selected = selected_ids();
        if (selected.size() > 1)
            emit layers_selected(selected);
        auto it = std::find_if(title_->layers.begin(), title_->layers.end(),
                               [&](const auto &layer) { return layer && layer->id == id; });
        if (it != title_->layers.end()) {
            int idx = (int)std::distance(title_->layers.begin(), it);
            can_move_down = idx > 0;
            can_move_up = idx < (int)title_->layers.size() - 1;
        }
        if (selected.size() <= 1)
            emit layer_selected(id);
    }
    if (!has_layer)
        emit layer_selected(std::string());

    if (btn_move_up_) btn_move_up_->setEnabled(can_move_up);
    if (btn_move_down_) btn_move_down_->setEnabled(can_move_down);
}

void LayerStack::on_add_text() { emit add_layer_requested(LayerType::Text); }
void LayerStack::on_add_clock() { emit add_layer_requested(LayerType::Clock); }
void LayerStack::on_add_ticker() { emit add_layer_requested(LayerType::Ticker); }
void LayerStack::on_add_rect() { emit add_layer_requested(LayerType::SolidRect); }
void LayerStack::on_add_image() { emit add_layer_requested(LayerType::Image); }

void LayerStack::on_move_up()
{
    std::string id = selected_id();
    if (!title_ || id.empty()) return;
    auto layer = title_->find_layer(id);
    if (!layer) return;
    title_->move_layer(id, +1);
    emit layer_order_changed();
    set_selected_layer(id);
}

void LayerStack::on_move_down()
{
    std::string id = selected_id();
    if (!title_ || id.empty()) return;
    auto layer = title_->find_layer(id);
    if (!layer) return;
    title_->move_layer(id, -1);
    emit layer_order_changed();
    set_selected_layer(id);
}

void LayerStack::on_delete()
{
    std::string id = selected_id();
    if (!id.empty()) emit delete_layer_requested(id);
}

void LayerStack::show_layer_context_menu(const QPoint &pos)
{
    if (!title_) return;

    QListWidgetItem *item = list_->itemAt(pos);
    std::string id = item ? item->data(Qt::UserRole).toString().toStdString() : selected_id();
    if (id.empty()) return;

    if (item && item->data(Qt::UserRole + 1).toString() == "layer")
        list_->setCurrentItem(item);

    QMenu menu(this);
    menu.setStyleSheet("QMenu{color:#ddd;background:#252525;border:1px solid #3a3a3a;}"
                       "QMenu::item{padding:5px 22px;}"
                       "QMenu::item:selected{background:#3b4f64;}"
                       "QMenu::item:disabled{color:#666;}");
    QAction *clone = menu.addAction(obsgs_tr("OBSTitles.CloneLayer"));
    QAction *copy = menu.addAction(obsgs_tr("OBSTitles.CopyLayer"));
    QAction *paste = menu.addAction(obsgs_tr("OBSTitles.PasteLayer"));
    paste->setEnabled(layer_clipboard_available_);
    menu.addSeparator();
    QAction *del = menu.addAction(obsgs_tr("OBSTitles.DeleteLayer"));

    QAction *chosen = menu.exec(list_->viewport()->mapToGlobal(pos));
    if (chosen == clone) emit clone_layer_requested(id);
    else if (chosen == copy) emit copy_layer_requested(id);
    else if (chosen == paste) emit paste_layer_requested(id);
    else if (chosen == del) emit delete_layer_requested(id);
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
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
}

void TimelineWidget::set_title(std::shared_ptr<Title> t)
{
    const bool title_changed = t != title_;
    title_ = t;
    if (title_changed) {
        scroll_x_ = 0;
        fit_on_next_resize_ = true;
        selected_keyframes_.clear();
    } else {
        prune_keyframe_selection();
    }
    clamp_scroll();
    clamp_vertical_scroll();
    if (fit_on_next_resize_ && width() > 40) {
        fit_on_next_resize_ = false;
        fit_timeline();
        return;
    }
    update();
}

void TimelineWidget::set_selected_layer(const std::string &lid)
{
    sel_layer_id_ = lid; update();
}

void TimelineWidget::set_playhead(double t)
{
    playhead_ = snap_time(t);
    if (title_)
        keep_playhead_visible();
    update();
}

void TimelineWidget::set_vertical_scroll(int scroll_y)
{
    scroll_y_ = scroll_y;
    clamp_vertical_scroll();
    update();
}

void TimelineWidget::set_pixels_per_sec(double pixels_per_sec, double anchor_time, int anchor_x)
{
    pixels_per_sec_ = std::clamp(pixels_per_sec, 5.0, 1200.0);
    scroll_x_ = (int)std::round(anchor_time * pixels_per_sec_) - anchor_x;
    clamp_scroll();
    keep_playhead_visible();
    update();
    emit zoom_percent_changed(zoom_percent());
}

void TimelineWidget::set_zoom_percent(int percent)
{
    int clamped = std::clamp(percent, 5, 1200);
    double anchor_time = title_ ? std::clamp(playhead_, 0.0, title_->duration) : playhead_;
    int anchor_x = std::clamp(time_to_x(anchor_time), 24, std::max(24, width() - 24));
    set_pixels_per_sec((double)clamped, anchor_time, anchor_x);
}

int TimelineWidget::zoom_percent() const
{
    return (int)std::round(pixels_per_sec_);
}

void TimelineWidget::fit_timeline()
{
    double dur = title_ ? std::max(obs_frame_duration(), title_->duration) : 10.0;
    double fitted = (double)std::max(1, width() - 40) / dur;
    set_pixels_per_sec(fitted, 0.0, 0);
}

bool TimelineWidget::has_selected_keyframes() const
{
    return title_ && !selected_keyframes_.empty();
}

bool TimelineWidget::has_keyframe_clipboard() const
{
    return !keyframe_clipboard_.empty();
}

bool TimelineWidget::copy_keyframe_selection()
{
    return copy_selected_keyframes();
}

bool TimelineWidget::cut_keyframe_selection()
{
    if (!cut_selected_keyframes()) return false;
    emit keyframe_easing_changed();
    return true;
}

bool TimelineWidget::delete_keyframe_selection()
{
    if (!delete_selected_keyframes()) return false;
    emit keyframe_easing_changed();
    return true;
}

bool TimelineWidget::paste_keyframes_at_playhead()
{
    if (!paste_keyframes_at(playhead_)) return false;
    emit keyframe_easing_changed();
    return true;
}

bool TimelineWidget::keep_playhead_visible()
{
    if (!title_) return false;
    int phx = time_to_x(playhead_);
    int old_scroll = scroll_x_;
    if (phx < 24)
        scroll_x_ = std::max(0, (int)std::round(playhead_ * pixels_per_sec_) - 24);
    if (phx > width() - 24)
        scroll_x_ = std::max(0, (int)std::round(playhead_ * pixels_per_sec_) - width() + 24);
    clamp_scroll();
    return old_scroll != scroll_x_;
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

int TimelineWidget::max_vertical_scroll() const
{
    int content_height = (int)timeline_rows(title_).size() * row_height();
    int viewport_height = std::max(0, height() - ruler_height());
    return std::max(0, content_height - viewport_height);
}

void TimelineWidget::clamp_vertical_scroll()
{
    scroll_y_ = std::clamp(scroll_y_, 0, max_vertical_scroll());
}


bool TimelineWidget::KeyframeRef::operator<(const KeyframeRef &other) const
{
    return std::tie(layer_id, prop_name, index) <
           std::tie(other.layer_id, other.prop_name, other.index);
}

AnimatedProperty *TimelineWidget::find_timeline_property(Layer &layer, const std::string &prop_name) const
{
    for (auto *prop : timeline_properties(layer)) {
        if (prop->name == prop_name)
            return prop;
    }
    return nullptr;
}

void TimelineWidget::clear_keyframe_selection()
{
    if (selected_keyframes_.empty()) return;
    selected_keyframes_.clear();
    update();
}

void TimelineWidget::prune_keyframe_selection()
{
    if (!title_) {
        selected_keyframes_.clear();
        return;
    }

    for (auto it = selected_keyframes_.begin(); it != selected_keyframes_.end();) {
        auto layer = title_->find_layer(it->layer_id);
        AnimatedProperty *prop = layer ? find_timeline_property(*layer, it->prop_name) : nullptr;
        if (!layer || !prop || it->index < 0 || it->index >= (int)prop->keyframes.size())
            it = selected_keyframes_.erase(it);
        else
            ++it;
    }
}

bool TimelineWidget::is_keyframe_selected(const std::string &layer_id, const std::string &prop_name, int kf_idx) const
{
    return selected_keyframes_.find({layer_id, prop_name, kf_idx}) != selected_keyframes_.end();
}

void TimelineWidget::select_keyframe(const std::string &layer_id, const std::string &prop_name,
                                     int kf_idx, bool additive, bool toggle)
{
    KeyframeRef ref{layer_id, prop_name, kf_idx};
    if (!additive)
        selected_keyframes_.clear();
    if (toggle && selected_keyframes_.find(ref) != selected_keyframes_.end())
        selected_keyframes_.erase(ref);
    else
        selected_keyframes_.insert(ref);
    update();
}

QRect TimelineWidget::marquee_rect() const
{
    return QRect(marquee_start_, marquee_current_).normalized()
        .intersected(QRect(0, ruler_height(), width(), std::max(0, height() - ruler_height())));
}

void TimelineWidget::select_keyframes_in_rect(const QRect &rect, bool additive)
{
    if (!title_) return;
    std::set<KeyframeRef> selection = additive ? selected_keyframes_ : std::set<KeyframeRef>{};
    auto rows = timeline_rows(title_);
    const QRect visible_timeline(0, ruler_height(), width(), std::max(0, height() - ruler_height()));
    QRect bounded = rect.normalized().intersected(visible_timeline);
    if (bounded.isEmpty()) {
        selected_keyframes_ = std::move(selection);
        update();
        return;
    }

    for (int row = 0; row < (int)rows.size(); ++row) {
        const auto &entry = rows[row];
        if (!entry.is_property || !entry.layer || !entry.prop) continue;
        if (!entry.layer->properties_expanded || entry.layer->locked) continue;
        int y = ruler_height() + row * row_height() - scroll_y_;
        int ky = y + row_height() / 2;
        if (ky < visible_timeline.top() || ky > visible_timeline.bottom()) continue;
        if (ky < bounded.top() || ky > bounded.bottom()) continue;
        for (int i = 0; i < (int)entry.prop->keyframes.size(); ++i) {
            const auto &kf = entry.prop->keyframes[i];
            int kx = time_to_x(entry.layer->in_time + kf.time);
            if (kx < visible_timeline.left() || kx > visible_timeline.right()) continue;
            if (bounded.contains(QPoint(kx, ky)))
                selection.insert({entry.layer->id, entry.prop->name, i});
        }
    }
    selected_keyframes_ = std::move(selection);
    update();
}

bool TimelineWidget::copy_selected_keyframes()
{
    if (!title_) return false;
    prune_keyframe_selection();
    if (selected_keyframes_.empty()) return false;

    struct PendingCopy {
        std::string layer_id;
        std::string prop_name;
        Keyframe keyframe;
        double timeline_time = 0.0;
    };
    std::vector<PendingCopy> pending;
    double origin = std::numeric_limits<double>::max();

    for (const auto &ref : selected_keyframes_) {
        auto layer = title_->find_layer(ref.layer_id);
        AnimatedProperty *prop = layer ? find_timeline_property(*layer, ref.prop_name) : nullptr;
        if (!layer || !prop || ref.index < 0 || ref.index >= (int)prop->keyframes.size()) continue;
        const Keyframe keyframe = prop->keyframes[ref.index];
        const double timeline_time = layer->in_time + keyframe.time;
        origin = std::min(origin, timeline_time);
        pending.push_back({ref.layer_id, ref.prop_name, keyframe, timeline_time});
    }

    if (pending.empty()) return false;
    std::sort(pending.begin(), pending.end(), [](const PendingCopy &a, const PendingCopy &b) {
        return std::tie(a.timeline_time, a.layer_id, a.prop_name) <
               std::tie(b.timeline_time, b.layer_id, b.prop_name);
    });

    keyframe_clipboard_.clear();
    keyframe_clipboard_.reserve(pending.size());
    for (const auto &entry : pending)
        keyframe_clipboard_.push_back({entry.layer_id, entry.prop_name, entry.keyframe, entry.timeline_time - origin});
    return true;
}

bool TimelineWidget::delete_selected_keyframes()
{
    if (!title_) return false;
    prune_keyframe_selection();
    if (selected_keyframes_.empty()) return false;

    std::map<std::pair<std::string, std::string>, std::vector<int>> grouped;
    for (const auto &ref : selected_keyframes_)
        grouped[{ref.layer_id, ref.prop_name}].push_back(ref.index);

    bool changed = false;
    for (auto &[prop_ref, indices] : grouped) {
        auto layer = title_->find_layer(prop_ref.first);
        if (!layer || layer->locked) continue;
        AnimatedProperty *prop = find_timeline_property(*layer, prop_ref.second);
        if (!prop) continue;
        std::sort(indices.begin(), indices.end(), std::greater<int>());
        for (int index : indices) {
            if (index < 0 || index >= (int)prop->keyframes.size()) continue;
            prop->keyframes.erase(prop->keyframes.begin() + index);
            changed = true;
        }
    }

    if (changed) {
        selected_keyframes_.clear();
        update();
    }
    return changed;
}

bool TimelineWidget::cut_selected_keyframes()
{
    if (!copy_selected_keyframes()) return false;
    return delete_selected_keyframes();
}

bool TimelineWidget::paste_keyframes_at(double timeline_time)
{
    if (!title_ || keyframe_clipboard_.empty()) return false;

    std::map<std::pair<std::string, std::string>, std::vector<double>> inserted_times;
    bool changed = false;
    const double paste_origin = std::clamp(snap_time(timeline_time), 0.0, title_->duration);

    for (const auto &entry : keyframe_clipboard_) {
        auto layer = title_->find_layer(entry.layer_id);
        if (!layer || layer->locked) continue;
        AnimatedProperty *prop = find_timeline_property(*layer, entry.prop_name);
        if (!prop) continue;

        Keyframe pasted = entry.keyframe;
        const double target_time = paste_origin + entry.offset;
        pasted.time = std::clamp(snap_time(target_time - layer->in_time),
                                 0.0, std::max(0.0, layer->out_time - layer->in_time));
        prop->keyframes.push_back(pasted);
        inserted_times[{entry.layer_id, entry.prop_name}].push_back(pasted.time);
        changed = true;
    }

    if (!changed) return false;

    selected_keyframes_.clear();
    for (auto &[prop_ref, times] : inserted_times) {
        auto layer = title_->find_layer(prop_ref.first);
        AnimatedProperty *prop = layer ? find_timeline_property(*layer, prop_ref.second) : nullptr;
        if (!prop) continue;
        std::sort(prop->keyframes.begin(), prop->keyframes.end(),
                  [](const Keyframe &a, const Keyframe &b) { return a.time < b.time; });

        std::set<int> used;
        for (double inserted_time : times) {
            int best = -1;
            double best_distance = std::numeric_limits<double>::max();
            for (int i = 0; i < (int)prop->keyframes.size(); ++i) {
                if (used.count(i)) continue;
                const double distance = std::abs(prop->keyframes[i].time - inserted_time);
                if (distance < best_distance) {
                    best = i;
                    best_distance = distance;
                }
            }
            if (best >= 0) {
                used.insert(best);
                selected_keyframes_.insert({prop_ref.first, prop_ref.second, best});
            }
        }
    }

    update();
    return true;
}

void TimelineWidget::begin_keyframe_drag(const std::string &layer_id, const std::string &prop_name,
                                         int kf_idx, double start_time)
{
    drag_mode_ = DragMode::Keyframe;
    drag_layer_id_ = layer_id;
    drag_prop_name_ = prop_name;
    drag_keyframe_index_ = kf_idx;
    drag_start_time_ = start_time;
    dragged_keyframes_.clear();
    prune_keyframe_selection();
    if (!is_keyframe_selected(layer_id, prop_name, kf_idx))
        selected_keyframes_ = {{layer_id, prop_name, kf_idx}};

    for (const auto &ref : selected_keyframes_) {
        auto layer = title_ ? title_->find_layer(ref.layer_id) : nullptr;
        if (!layer || layer->locked) continue;
        AnimatedProperty *prop = find_timeline_property(*layer, ref.prop_name);
        if (!prop || ref.index < 0 || ref.index >= (int)prop->keyframes.size()) continue;
        dragged_keyframes_.push_back({ref, prop->keyframes[ref.index].time});
    }
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
                p.drawText(loop_x0 + 4, 20, 80, 16, Qt::AlignVCenter, obsgs_tr("OBSTitles.LoopIn"));
                p.drawText(loop_x1 + 4, 20, 80, 16, Qt::AlignVCenter, obsgs_tr("OBSTitles.LoopOut"));
            }
        }
        if (title_->playback_mode == 2) {
            int pause_x = time_to_x(std::clamp(title_->pause_time, 0.0, dur));
            p.setPen(QPen(QColor(0xff, 0xc8, 0x32), 2));
            p.drawLine(pause_x, 12, pause_x, rh);
            p.setBrush(QColor(0xff, 0xc8, 0x32));
            p.setPen(Qt::NoPen);
            QPolygon marker;
            marker << QPoint(pause_x - 6, 12) << QPoint(pause_x + 6, 12) << QPoint(pause_x, 22);
            p.drawPolygon(marker);
            p.setPen(QColor(0xff, 0xe0, 0x85));
            p.drawText(pause_x + 4, 22, 100, 16, Qt::AlignVCenter, obsgs_tr("OBSTitles.Pause"));
            p.setBrush(Qt::NoBrush);
        }
    }

    /* Layer/property rows.  This uses the same row model as LayerStack so
     * keyframed property rows stay vertically aligned with the layer list.
     */
    auto rows = timeline_rows(title_);
    for (int row = 0; row < (int)rows.size(); ++row) {
        auto &entry = rows[row];
        auto &layer = entry.layer;
        int y = rh + row * rowh - scroll_y_;
        if (y > H) break;
        if (y + rowh < rh) continue;
        bool sel = (layer->id == sel_layer_id_);

        p.fillRect(0, y, W, rowh,
                   entry.is_property ? QColor(0x19,0x19,0x19) :
                   sel ? QColor(0x1e,0x3a,0x5a) : QColor(0x1e,0x1e,0x1e));
        p.setPen(QColor(0x2a,0x2a,0x2a));
        p.drawLine(0, y + rowh - 1, W, y + rowh - 1);

        int x0 = time_to_x(layer->in_time);
        int x1 = time_to_x(layer->out_time);
        if (!entry.is_property) {
            QRect strip_rect(std::min(x0, x1), y + 3, std::abs(x1 - x0), rowh - 6);
            QColor bar_col = layer_color(*layer, row);
            if (!layer->visible) {
                const int gray = qGray(bar_col.rgb());
                bar_col = QColor(gray, gray, gray).darker(135);
            }
            if (sel) bar_col = bar_col.lighter(125);
            p.fillRect(strip_rect, bar_col);
            if (layer->locked) {
                p.save();
                p.setClipRect(strip_rect);
                p.setPen(QPen(QColor(0x09, 0x09, 0x09, 170), 2));
                for (int lx = strip_rect.left() - strip_rect.height(); lx < strip_rect.right() + strip_rect.height(); lx += 8)
                    p.drawLine(lx, strip_rect.bottom(), lx + strip_rect.height(), strip_rect.top());
                p.restore();
            }
            p.setBrush(Qt::NoBrush);
            p.setPen(QColor(0x0d,0x0d,0x0d));
            p.drawRect(strip_rect);

            /* Trim handles for mouse resizing of unlocked layer in/out. */
            if (!layer->locked) {
                p.fillRect(x0, y + 3, 4, rowh - 6, QColor(0xdc,0xdc,0xdc,150));
                p.fillRect(x1 - 4, y + 3, 4, rowh - 6, QColor(0xdc,0xdc,0xdc,150));
            }

            p.setPen(layer->visible ? QColor(0xcc,0xcc,0xcc) : QColor(0x8a,0x8a,0x8a));
            p.drawText(std::max(strip_rect.left(), 0) + 6, y, std::max(1, strip_rect.width() - 12), rowh,
                       Qt::AlignVCenter, QString::fromStdString(layer->name));
        } else {
            p.fillRect(x0, y + rowh / 2 - 1, x1 - x0, 2, QColor(0x36,0x36,0x36));
            p.setPen(QColor(0x77,0x77,0x77));
            p.drawText(6, y, 150, rowh, Qt::AlignVCenter, property_label(entry.prop->name));
        }

        auto draw_kf = [&](const AnimatedProperty &prop) {
            for (int i = 0; i < (int)prop.keyframes.size(); ++i) {
                const auto &kf = prop.keyframes[i];
                int kx = time_to_x(layer->in_time + kf.time);
                if (kx < 0 || kx > W) continue;
                int ky = y + rowh / 2;
                QColor kf_fill = keyframe_color(kf.easing);
                if (!layer->visible)
                    kf_fill = kf_fill.darker(160);
                const bool selected = is_keyframe_selected(layer->id, prop.name, i);
                if (selected) {
                    draw_keyframe_marker(p, QPointF(kx, ky), kf.easing, 8.0,
                                         QColor(0xff, 0xff, 0xff, 45),
                                         QColor(0xff, 0xff, 0xff), 2.0);
                }
                draw_keyframe_marker(p, QPointF(kx, ky), kf.easing, 5.0,
                                     selected ? kf_fill.lighter(125) : kf_fill,
                                     selected ? QColor(0xff, 0xff, 0xff) : QColor(0x7a, 0x5a, 0x00),
                                     selected ? 2.0 : 1.0);
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

    if (drag_mode_ == DragMode::Marquee && marquee_moved_) {
        QRect rect = marquee_rect();
        p.fillRect(rect, QColor(0x00, 0x78, 0xd4, 35));
        p.setPen(QPen(QColor(0x70, 0xc8, 0xff), 1, Qt::DashLine));
        p.setBrush(Qt::NoBrush);
        p.drawRect(rect.adjusted(0, 0, -1, -1));
    }
}

bool TimelineWidget::hit_keyframe(const QPoint &pos, std::shared_ptr<Layer> *hit_layer,
                                  AnimatedProperty **hit_prop, int *hit_kf_idx,
                                  int *hit_row_idx) const
{
    if (!title_ || pos.y() < ruler_height()) return false;
    auto rows = timeline_rows(title_);
    int row = (pos.y() - ruler_height() + scroll_y_) / row_height();
    if (row < 0 || row >= (int)rows.size()) return false;

    auto &entry = rows[row];
    constexpr int kHitRadius = 7;
    auto test_prop = [&](AnimatedProperty *prop) -> bool {
        for (int i = 0; i < (int)prop->keyframes.size(); ++i) {
            const auto &kf = prop->keyframes[i];
            int kx = time_to_x(entry.layer->in_time + kf.time);
            int ky = ruler_height() + row * row_height() - scroll_y_ + row_height() / 2;
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
    const bool has_hit = hit_keyframe(ev->pos(), &layer, &hit_prop, &hit_idx, nullptr);
    if (has_hit && layer && layer->locked) return;
    if (!has_hit && keyframe_clipboard_.empty()) return;

    if (has_hit && layer && hit_prop && !is_keyframe_selected(layer->id, hit_prop->name, hit_idx))
        select_keyframe(layer->id, hit_prop->name, hit_idx, false, false);
    prune_keyframe_selection();

    QMenu menu(this);
    menu.setTitle(has_hit ? obsgs_tr("OBSTitles.KeyframeEasing") : obsgs_tr("OBSTitles.Paste"));

    QAction *copy_action = menu.addAction(obsgs_tr("OBSTitles.Copy"));
    QAction *cut_action = menu.addAction(obsgs_tr("OBSTitles.Cut"));
    QAction *paste_action = menu.addAction(obsgs_tr("OBSTitles.Paste"));
    QAction *delete_action = menu.addAction(obsgs_tr("OBSTitles.Delete"));
    const bool has_selection = !selected_keyframes_.empty();
    copy_action->setEnabled(has_selection);
    cut_action->setEnabled(has_selection);
    paste_action->setEnabled(!keyframe_clipboard_.empty());
    delete_action->setEnabled(has_selection);

    struct EasingChoice {
        QAction *action = nullptr;
        AnimatedProperty *prop = nullptr;
        std::vector<int> target_indices;
        EasingType easing = EasingType::Linear;
    };
    std::vector<EasingChoice> choices;

    auto swatch_icon = [](EasingType easing) {
        QPixmap swatch(16, 16);
        swatch.fill(Qt::transparent);
        QPainter painter(&swatch);
        draw_keyframe_marker(painter, QPointF(8, 8), easing, 5.0,
                             keyframe_color(easing), QColor(0x7a, 0x5a, 0x00), 1.0);
        return QIcon(swatch);
    };

    auto add_easing_action = [&](QMenu *target_menu, const QString &label,
                                 AnimatedProperty *prop, EasingType easing,
                                 const std::vector<int> &indices) {
        QAction *action = target_menu->addAction(swatch_icon(easing), label);
        action->setToolTip(easing == EasingType::Hold
            ? obsgs_tr("OBSTitles.HoldEasingTooltip")
            : obsgs_tr("OBSTitles.EasingTooltip"));
        action->setCheckable(true);
        action->setChecked(prop && std::all_of(indices.begin(), indices.end(), [&](int idx) {
            return idx >= 0 && idx < (int)prop->keyframes.size() &&
                   prop->keyframes[idx].easing == easing;
        }));
        choices.push_back({action, prop, indices, easing});
        return action;
    };

    auto add_easing_group = [&](QMenu *target_menu, AnimatedProperty *prop, const std::vector<int> &indices) {
        auto *group = new QActionGroup(target_menu);
        group->setExclusive(true);
        for (auto [label, easing] : std::initializer_list<std::pair<QString, EasingType>>{
                 {obsgs_tr("OBSTitles.Linear"), EasingType::Linear},
                 {obsgs_tr("OBSTitles.EasyEase"), EasingType::EaseInOut},
                 {obsgs_tr("OBSTitles.EaseIn"), EasingType::EaseIn},
                 {obsgs_tr("OBSTitles.EaseOut"), EasingType::EaseOut},
                 {obsgs_tr("OBSTitles.Hold"), EasingType::Hold},
                 {obsgs_tr("OBSTitles.CustomBezier"), EasingType::Bezier},
             }) {
            add_easing_action(target_menu, label, prop, easing, indices)->setActionGroup(group);
        }
    };

    if (has_hit && layer && hit_prop) {
        menu.addSeparator();
        QMenu *easing_menu = menu.addMenu(obsgs_tr("OBSTitles.Easing"));
        QAction *header = easing_menu->addAction(QString("%1 · %2")
            .arg(QString::fromStdString(layer->name))
            .arg(property_label(hit_prop->name)));
        header->setEnabled(false);

        const bool has_previous_segment = hit_idx > 0;
        const bool has_next_segment = hit_idx + 1 < (int)hit_prop->keyframes.size();
        if (!has_previous_segment && !has_next_segment) {
            QAction *message = easing_menu->addAction(obsgs_tr("OBSTitles.AddKeyframeForEasing"));
            message->setEnabled(false);
        } else {
            QAction *scope = easing_menu->addAction(has_previous_segment && has_next_segment
                ? obsgs_tr("OBSTitles.EasingBothSegments")
                : has_next_segment ? obsgs_tr("OBSTitles.EasingNextSegment") : obsgs_tr("OBSTitles.EasingPreviousSegment"));
            scope->setEnabled(false);
            easing_menu->addSeparator();

            auto default_targets = [&]() {
                std::vector<int> indices;
                if (has_previous_segment && has_next_segment) {
                    indices = {hit_idx - 1, hit_idx};
                } else if (has_next_segment) {
                    indices = {hit_idx};
                } else {
                    indices = {hit_idx - 1};
                }
                return indices;
            };
            add_easing_group(easing_menu, hit_prop, default_targets());

            if (has_previous_segment && has_next_segment) {
                easing_menu->addSeparator();
                QMenu *advanced = easing_menu->addMenu(obsgs_tr("OBSTitles.ApplyOneSide"));
                QMenu *previous = advanced->addMenu(obsgs_tr("OBSTitles.PreviousSegment"));
                add_easing_group(previous, hit_prop, {hit_idx - 1});
                QMenu *next = advanced->addMenu(obsgs_tr("OBSTitles.NextSegment"));
                add_easing_group(next, hit_prop, {hit_idx});
            }
        }
    }

    QAction *chosen = menu.exec(ev->globalPos());
    if (!chosen) return;

    if (chosen == copy_action) {
        copy_selected_keyframes();
        return;
    }
    if (chosen == cut_action) {
        if (cut_selected_keyframes()) emit keyframe_easing_changed();
        return;
    }
    if (chosen == paste_action) {
        if (paste_keyframes_at(std::clamp(x_to_time(ev->pos().x()), 0.0, title_->duration)))
            emit keyframe_easing_changed();
        return;
    }
    if (chosen == delete_action) {
        if (delete_selected_keyframes()) emit keyframe_easing_changed();
        return;
    }

    auto choice = std::find_if(choices.begin(), choices.end(),
                               [&](const EasingChoice &candidate) { return candidate.action == chosen; });
    if (choice == choices.end() || !choice->prop) return;

    for (int idx : choice->target_indices) {
        if (idx >= 0 && idx < (int)choice->prop->keyframes.size())
            apply_easing_preset(choice->prop->keyframes[idx], choice->easing);
    }
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

    if (ev->modifiers() & Qt::ControlModifier) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        int cursor_x = (int)std::round(ev->position().x());
#else
        int cursor_x = ev->pos().x();
#endif
        double anchor_time = (cursor_x + scroll_x_) / pixels_per_sec_;
        int delta = angle.y() != 0 ? angle.y() : angle.x();
        if (delta == 0) return;

        double factor = std::pow(1.0015, delta);
        set_pixels_per_sec(pixels_per_sec_ * factor, anchor_time, cursor_x);
        ev->accept();
        return;
    }

    int delta = angle.y() != 0 ? -angle.y() : -angle.x();
    if (delta == 0) return;
    emit vertical_scroll_delta_requested(delta);
    ev->accept();
}

void TimelineWidget::resizeEvent(QResizeEvent *ev)
{
    QWidget::resizeEvent(ev);
    if (fit_on_next_resize_ && title_ && width() > 40) {
        fit_on_next_resize_ = false;
        fit_timeline();
        return;
    }
    clamp_scroll();
    clamp_vertical_scroll();
}

void TimelineWidget::keyPressEvent(QKeyEvent *ev)
{
    if (!title_) {
        QWidget::keyPressEvent(ev);
        return;
    }

    if (ev->matches(QKeySequence::Copy) && has_selected_keyframes()) {
        copy_keyframe_selection();
        ev->accept();
        return;
    }
    if (ev->matches(QKeySequence::Cut) && has_selected_keyframes()) {
        cut_keyframe_selection();
        ev->accept();
        return;
    }
    if (ev->matches(QKeySequence::Paste) && has_keyframe_clipboard()) {
        paste_keyframes_at_playhead();
        ev->accept();
        return;
    }
    if ((ev->key() == Qt::Key_Delete || ev->key() == Qt::Key_Backspace) && has_selected_keyframes()) {
        delete_keyframe_selection();
        ev->accept();
        return;
    }

    QWidget::keyPressEvent(ev);
}

void TimelineWidget::mousePressEvent(QMouseEvent *ev)
{
    setFocus(Qt::MouseFocusReason);
    if (!title_) return;
    drag_mode_ = DragMode::None;
    drag_layer_id_.clear();
    drag_prop_name_.clear();
    drag_keyframe_index_ = -1;
    drag_start_time_ = 0.0;
    drag_start_in_ = 0.0;
    drag_start_out_ = 0.0;
    dragged_keyframes_.clear();
    marquee_moved_ = false;

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
        if (hit_layer && hit_layer->locked) {
            ev->accept();
            return;
        }
        if (hit_layer) emit layer_selected(hit_layer->id);
        const bool shift = ev->modifiers() & Qt::ShiftModifier;
        if (shift) {
            select_keyframe(hit_layer->id, hit_prop->name, hit_idx, true, true);
            ev->accept();
            return;
        }
        if (!is_keyframe_selected(hit_layer->id, hit_prop->name, hit_idx))
            select_keyframe(hit_layer->id, hit_prop->name, hit_idx, false, false);
        begin_keyframe_drag(hit_layer->id, hit_prop->name, hit_idx,
                            std::clamp(x_to_time(ev->pos().x()), 0.0, title_->duration));
        setCursor(Qt::ClosedHandCursor);
        ev->accept();
        return;
    }

    auto rows = timeline_rows(title_);
    int row = (ev->pos().y() - ruler_height() + scroll_y_) / row_height();
    if (row >= 0 && row < (int)rows.size() && !rows[row].is_property) {
        auto layer = rows[row].layer;
        if (!layer) {
            ev->accept();
            return;
        }
        int x0 = time_to_x(layer->in_time);
        int x1 = time_to_x(layer->out_time);
        constexpr int kTrimHit = 7;
        const bool hit_strip = ev->pos().x() >= std::min(x0, x1) - kTrimHit &&
                               ev->pos().x() <= std::max(x0, x1) + kTrimHit;
        if (layer->locked && hit_strip) {
            ev->accept();
            return;
        }
        if (hit_strip) emit layer_selected(layer->id);
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

    if (ev->button() == Qt::LeftButton && ev->pos().y() >= ruler_height()) {
        drag_mode_ = DragMode::Marquee;
        marquee_start_ = ev->pos();
        marquee_current_ = ev->pos();
        marquee_additive_ = ev->modifiers() & Qt::ShiftModifier;
        marquee_moved_ = false;
        if (!marquee_additive_)
            selected_keyframes_.clear();
        ev->accept();
        update();
        return;
    }

    emit layer_selected(std::string());
    ev->accept();
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
        double delta = t - drag_start_time_;
        for (const auto &dragged : dragged_keyframes_) {
            auto layer = title_->find_layer(dragged.ref.layer_id);
            if (!layer || layer->locked) continue;
            AnimatedProperty *prop = find_timeline_property(*layer, dragged.ref.prop_name);
            if (!prop || dragged.ref.index < 0 || dragged.ref.index >= (int)prop->keyframes.size()) continue;
            prop->keyframes[dragged.ref.index].time =
                std::clamp(dragged.start_time + delta, 0.0, std::max(0.0, layer->out_time - layer->in_time));
        }
        update();
        return;
    }

    if (drag_mode_ == DragMode::Marquee) {
        marquee_current_ = ev->pos();
        if ((marquee_current_ - marquee_start_).manhattanLength() >= 3)
            marquee_moved_ = true;
        update();
        return;
    }

    if (drag_mode_ == DragMode::TrimIn || drag_mode_ == DragMode::TrimOut) {
        auto layer = title_->find_layer(drag_layer_id_);
        if (!layer || layer->locked) return;
        if (drag_mode_ == DragMode::TrimIn)
            layer->in_time = std::clamp(t, 0.0, std::max(0.0, layer->out_time - obs_frame_duration()));
        else
            layer->out_time = std::clamp(t, layer->in_time + obs_frame_duration(), title_->duration);
        update();
        return;
    }

    if (drag_mode_ == DragMode::Layer) {
        auto layer = title_->find_layer(drag_layer_id_);
        if (!layer || layer->locked) return;
        double duration = std::max(obs_frame_duration(), drag_start_out_ - drag_start_in_);
        double new_in = drag_start_in_ + (t - drag_start_time_);
        new_in = std::clamp(new_in, 0.0, std::max(0.0, title_->duration - duration));
        layer->in_time = new_in;
        layer->out_time = std::min(title_->duration, new_in + duration);
        update();
        return;
    }

    auto rows = timeline_rows(title_);
    int row = (ev->pos().y() - ruler_height() + scroll_y_) / row_height();
    if (row >= 0 && row < (int)rows.size() && !rows[row].is_property) {
        if (!rows[row].layer || rows[row].layer->locked) {
            unsetCursor();
            return;
        }
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

    if (drag_mode_ == DragMode::Marquee) {
        if (marquee_moved_)
            select_keyframes_in_rect(marquee_rect(), marquee_additive_);
        else if (!marquee_additive_)
            clear_keyframe_selection();
    }

    if (drag_mode_ == DragMode::Keyframe && title_) {
        std::map<KeyframeRef, double> selected_times;
        for (const auto &ref : selected_keyframes_) {
            auto layer = title_->find_layer(ref.layer_id);
            AnimatedProperty *prop = layer ? find_timeline_property(*layer, ref.prop_name) : nullptr;
            if (prop && ref.index >= 0 && ref.index < (int)prop->keyframes.size())
                selected_times[ref] = prop->keyframes[ref.index].time;
        }

        std::set<std::pair<std::string, std::string>> props_to_sort;
        for (const auto &dragged : dragged_keyframes_)
            props_to_sort.insert({dragged.ref.layer_id, dragged.ref.prop_name});

        for (const auto &prop_ref : props_to_sort) {
            if (auto layer = title_->find_layer(prop_ref.first)) {
                if (layer->locked) continue;
                if (AnimatedProperty *prop = find_timeline_property(*layer, prop_ref.second)) {
                    std::sort(prop->keyframes.begin(), prop->keyframes.end(),
                              [](const Keyframe &a, const Keyframe &b) { return a.time < b.time; });
                }
            }
        }

        std::set<KeyframeRef> remapped;
        std::map<std::pair<std::string, std::string>, std::set<int>> used_indices;
        for (const auto &[ref, selected_time] : selected_times) {
            auto layer = title_->find_layer(ref.layer_id);
            AnimatedProperty *prop = layer ? find_timeline_property(*layer, ref.prop_name) : nullptr;
            if (!prop) continue;
            int best = -1;
            double best_distance = std::numeric_limits<double>::max();
            auto key = std::make_pair(ref.layer_id, ref.prop_name);
            for (int i = 0; i < (int)prop->keyframes.size(); ++i) {
                if (used_indices[key].count(i)) continue;
                double distance = std::abs(prop->keyframes[i].time - selected_time);
                if (distance < best_distance) {
                    best = i;
                    best_distance = distance;
                }
            }
            if (best >= 0) {
                used_indices[key].insert(best);
                remapped.insert({ref.layer_id, ref.prop_name, best});
            }
        }
        selected_keyframes_ = std::move(remapped);
    }

    drag_mode_ = DragMode::None;
    drag_layer_id_.clear();
    drag_prop_name_.clear();
    drag_keyframe_index_ = -1;
    drag_start_time_ = 0.0;
    drag_start_in_ = 0.0;
    drag_start_out_ = 0.0;
    dragged_keyframes_.clear();
    marquee_additive_ = false;
    marquee_moved_ = false;
    unsetCursor();
    update();
    if (changed) emit keyframe_easing_changed();
}

/* ══════════════════════════════════════════════════════════════════
 *  TitlePropertiesPanel
 * ══════════════════════════════════════════════════════════════════ */
TitlePropertiesPanel::TitlePropertiesPanel(QWidget *parent)
    : QGroupBox(obsgs_tr("OBSTitles.TitleSection"), parent)
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

    auto add_form_row = [this](QFormLayout *form, const QString &label_text, QWidget *field) {
        auto *label = new NumericDragLabel(label_text, field, form->parentWidget(),
                                           [this]() {
                                               if (loading_values_) return;
                                               numeric_label_dragging_ = true;
                                               emit title_changed(true);
                                           },
                                           [this]() {
                                               if (loading_values_) return;
                                               numeric_label_dragging_ = false;
                                               emit title_changed(true);
                                           });
        label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        form->addRow(label, field);
    };

    cmb_playback_mode_ = new QComboBox(this);
    cmb_playback_mode_->addItem(obsgs_tr("OBSTitles.PlayOnce"), 0);
    cmb_playback_mode_->addItem(obsgs_tr("OBSTitles.LoopInOut"), 1);
    cmb_playback_mode_->addItem(obsgs_tr("OBSTitles.PauseAtTimelinePosition"), 2);
    add_form_row(fl, obsgs_tr("OBSTitles.PlaybackModeLabel"), cmb_playback_mode_);

    cmb_loop_type_ = new QComboBox(this);
    cmb_loop_type_->addItem(obsgs_tr("OBSTitles.RestartLoop"), 0);
    cmb_loop_type_->addItem(obsgs_tr("OBSTitles.PingPongLoop"), 1);
    add_form_row(fl, obsgs_tr("OBSTitles.LoopTypeLabel"), cmb_loop_type_);

    spn_pause_frame_ = new QSpinBox(this);
    spn_pause_frame_->setRange(0, 1000000);
    spn_pause_frame_->setToolTip(obsgs_tr("OBSTitles.PauseFrameTooltip"));
    add_form_row(fl, obsgs_tr("OBSTitles.PauseFrameLabel"), spn_pause_frame_);


    spn_duration_ = new QDoubleSpinBox(this);
    spn_duration_->setRange(0.1, 3600.0);
    spn_duration_->setSingleStep(0.5);
    spn_duration_->setDecimals(2);
    spn_duration_->setSuffix(" s");
    add_form_row(fl, obsgs_tr("OBSTitles.LengthLabel"), spn_duration_);

    spn_loop_start_ = new QDoubleSpinBox(this);
    spn_loop_start_->setRange(0.0, 3600.0);
    spn_loop_start_->setSingleStep(0.5);
    spn_loop_start_->setDecimals(2);
    spn_loop_start_->setSuffix(" s");
    spn_loop_start_->setToolTip(obsgs_tr("OBSTitles.LoopStartTooltip"));
    add_form_row(fl, obsgs_tr("OBSTitles.LoopStartLabel"), spn_loop_start_);

    spn_loop_end_ = new QDoubleSpinBox(this);
    spn_loop_end_->setRange(0.0, 3600.0);
    spn_loop_end_->setSingleStep(0.5);
    spn_loop_end_->setDecimals(2);
    spn_loop_end_->setSuffix(" s");
    spn_loop_end_->setToolTip(obsgs_tr("OBSTitles.LoopEndTooltip"));
    add_form_row(fl, obsgs_tr("OBSTitles.LoopEndLabel"), spn_loop_end_);

    connect(cmb_playback_mode_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) {
                if (!title_ || loading_values_) return;
                title_->playback_mode = cmb_playback_mode_->currentData().toInt();
                if (title_->playback_mode == 2 && title_->pause_time <= 0.0)
                    title_->pause_time = title_->duration;
                load_values();
                emit title_changed(!numeric_label_dragging_);
            });

    connect(cmb_loop_type_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) {
                if (!title_ || loading_values_) return;
                title_->loop_type = cmb_loop_type_->currentData().toInt();
                emit title_changed(!numeric_label_dragging_);
            });

    connect(spn_pause_frame_, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [this](int frame) {
                if (!title_ || loading_values_) return;
                title_->pause_time = std::clamp(frame * obs_frame_duration(), 0.0, title_->duration);
                load_values();
                emit title_changed(!numeric_label_dragging_);
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
                emit title_changed(!numeric_label_dragging_);
            });

    connect(spn_loop_start_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this](double v) {
                if (!title_ || loading_values_) return;
                title_->loop_start = std::clamp(v, 0.0, title_->duration);
                title_->loop_end = std::clamp(title_->loop_end, title_->loop_start, title_->duration);
                load_values();
                emit title_changed(!numeric_label_dragging_);
            });

    connect(spn_loop_end_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this](double v) {
                if (!title_ || loading_values_) return;
                title_->loop_end = std::clamp(v, title_->loop_start, title_->duration);
                load_values();
                emit title_changed(!numeric_label_dragging_);
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
    loading_values_ = false;
}

/* ══════════════════════════════════════════════════════════════════
 *  PropertiesPanel
 * ══════════════════════════════════════════════════════════════════ */
PropertiesPanel::PropertiesPanel(QWidget *parent) : QScrollArea(parent)
{
    setWidgetResizable(true);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setStyleSheet("QScrollArea{background:#151515;border:none;}");

    auto *inner = new QWidget(this);
    inner->setStyleSheet("background:#151515;");
    auto *vl = new QVBoxLayout(inner);
    vl->setContentsMargins(4, 4, 4, 4);
    vl->setSpacing(3);

    /* Header */
    auto *hdr = new QLabel(obsgs_tr("OBSTitles.Properties"), inner);
    hdr->setStyleSheet("color:#888;font-size:9px;font-weight:bold;letter-spacing:1px;padding:2px 4px;");
    vl->addWidget(hdr);

    const QString section_style =
        "QGroupBox{color:#d0d0d0;background:#1b1b1b;border:1px solid #303030;"
        "border-radius:2px;margin-top:16px;font-size:10px;font-weight:bold;}"
        "QGroupBox::title{subcontrol-origin:margin;left:6px;top:2px;padding:0 4px;}"
        "QGroupBox::indicator{width:10px;height:10px;margin-left:2px;}"
        "QLabel{color:#a9a9a9;font-size:10px;}";
    const QString control_style =
        "QDoubleSpinBox,QSpinBox,QComboBox,QLineEdit,QTextEdit{color:#ddd;background:#252525;"
        "border:1px solid #363636;border-radius:2px;padding:1px 3px;selection-background-color:#4b6ea8;}"
        "QDoubleSpinBox:focus,QSpinBox:focus,QComboBox:focus,QLineEdit:focus,QTextEdit:focus{border-color:#5a78ad;}";

    auto style_form = [](QFormLayout *form) {
        form->setContentsMargins(6, 5, 6, 6);
        form->setHorizontalSpacing(5);
        form->setVerticalSpacing(3);
        form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
        form->setFormAlignment(Qt::AlignTop);
        form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    };

    auto add_form_row = [this](QFormLayout *form, const QString &label_text, QWidget *field) {
        if (!form || label_text.isEmpty()) {
            if (form) form->addRow(label_text, field);
            return;
        }

        auto *label = new NumericDragLabel(label_text, field, form->parentWidget(),
                                           [this]() {
                                               if (loading_values_) return;
                                               numeric_label_dragging_ = true;
                                               emit property_changed(true);
                                           },
                                           [this]() {
                                               if (loading_values_) return;
                                               numeric_label_dragging_ = false;
                                               emit property_changed(true);
                                           });
        label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        form->addRow(label, field);
    };

    const QString checkbox_style =
        "QCheckBox{color:#d8d8d8;font-size:10px;spacing:5px;}"
        "QCheckBox::indicator{width:13px;height:13px;border:1px solid #3a3a3a;"
        "background:#242424;border-radius:2px;}"
        "QCheckBox::indicator:hover{border-color:#4a4a4a;background:#303030;}"
        "QCheckBox::indicator:checked{background:#4b6ea8;border-color:#6f8fc4;}";
    const QString push_button_style =
        "QPushButton{color:#d8d8d8;background:#242424;border:1px solid #373737;"
        "border-radius:2px;font-size:10px;padding:2px 8px;}"
        "QPushButton:hover{background:#303030;border-color:#4a4a4a;}"
        "QPushButton:pressed{background:#4b6ea8;color:white;border-color:#6f8fc4;}";

    auto style_checkbox = [&](QCheckBox *box) {
        box->setFixedHeight(22);
        box->setStyleSheet(checkbox_style);
    };
    auto style_push_button = [&](QPushButton *button) {
        button->setFixedHeight(22);
        button->setStyleSheet(push_button_style);
    };

    /* ── Transform ── */
    auto *tform_box = new QGroupBox(obsgs_tr("OBSTitles.Transform"), inner);
    tform_box->setStyleSheet(section_style);
    auto *tfl = new QFormLayout(tform_box);
    style_form(tfl);

    auto mk_dspin = [&](double lo, double hi, double step) {
        auto *s = new QDoubleSpinBox(inner);
        s->setRange(lo, hi);
        s->setSingleStep(step);
        s->setDecimals(1);
        s->setFixedHeight(22);
        s->setStyleSheet(control_style);
        return s;
    };

    auto mk_kf_button = [&](const QString &tip) {
        auto *b = new QPushButton(inner);
        b->setFixedSize(22, 22);
        b->setIconSize(QSize(16, 16));
        b->setIcon(keyframe_diamond_icon(false));
        b->setToolTip(tip);
        b->setAccessibleName(tip);
        b->setProperty("active", false);
        b->setProperty("outlined", false);
        b->setStyleSheet("QPushButton{background:transparent;border:none;border-radius:2px;padding:0;}"
                         "QPushButton:hover{background:#303030;}"
                         "QPushButton[outlined=\"true\"]{background:#201d12;}"
                         "QPushButton[active=\"true\"]{background:#2b2518;}");
        return b;
    };

    auto with_kf = [&](QWidget *field, QPushButton *button) {
        auto *row = new QWidget(inner);
        auto *hl = new QHBoxLayout(row);
        hl->setContentsMargins(0, 0, 0, 0);
        hl->setSpacing(3);
        field->setSizePolicy(QSizePolicy::Expanding, field->sizePolicy().verticalPolicy());
        hl->addWidget(button);
        hl->addWidget(field, 1);
        return row;
    };

    auto make_collapsible = [this](QGroupBox *box) {
        box->setCheckable(true);
        box->setChecked(true);
        QObject::connect(box, &QGroupBox::toggled, box, [this, box](bool expanded) {
            const int scroll = verticalScrollBar() ? verticalScrollBar()->value() : 0;
            if (auto *form = qobject_cast<QFormLayout *>(box->layout())) {
                for (int row = 0; row < form->rowCount(); ++row) {
                    for (auto role : {QFormLayout::LabelRole, QFormLayout::FieldRole}) {
                        if (auto *item = form->itemAt(row, role)) {
                            if (auto *widget = item->widget()) widget->setVisible(expanded);
                            if (auto *child_layout = item->layout()) {
                                for (int j = 0; j < child_layout->count(); ++j)
                                    if (auto *child = child_layout->itemAt(j)->widget()) child->setVisible(expanded);
                            }
                        }
                    }
                }
            } else if (box->layout()) {
                for (int i = 0; i < box->layout()->count(); ++i)
                    if (auto *widget = box->layout()->itemAt(i)->widget()) widget->setVisible(expanded);
            }
            QTimer::singleShot(0, this, [this, scroll]() {
                if (verticalScrollBar()) verticalScrollBar()->setValue(scroll);
            });
        });
    };

    spn_px_      = mk_dspin(-9999, 9999, 1.0);
    spn_py_      = mk_dspin(-9999, 9999, 1.0);
    spn_scale_x_ = mk_dspin(-10000.0, 10000.0, 1.0);
    spn_scale_y_ = mk_dspin(-10000.0, 10000.0, 1.0);
    spn_scale_x_->setSuffix("%");
    spn_scale_y_->setSuffix("%");
    chk_scale_lock_ = new QCheckBox(obsgs_tr("OBSTitles.ScaleLock"), inner);
    chk_scale_lock_->setChecked(true);
    style_checkbox(chk_scale_lock_);
    spn_rot_     = mk_dspin(-9999,  9999,  0.5);
    spn_opacity_ = mk_dspin(0.0,   1.0,  0.01);
    spn_origin_x_ = mk_dspin(0.0, 1.0, 0.05);
    spn_origin_y_ = mk_dspin(0.0, 1.0, 0.05);
    spn_origin_x_->setDecimals(2);
    spn_origin_y_->setDecimals(2);
    spn_origin_x_->setToolTip(obsgs_tr("OBSTitles.OriginXTooltip"));
    spn_origin_y_->setToolTip(obsgs_tr("OBSTitles.OriginYTooltip"));
    cmb_anchor_ = new QComboBox(inner);
    for (const QString &label : QStringList{obsgs_tr("OBSTitles.TopLeft"), obsgs_tr("OBSTitles.TopCenter"), obsgs_tr("OBSTitles.TopRight"), obsgs_tr("OBSTitles.CenterLeft"), obsgs_tr("OBSTitles.Center"), obsgs_tr("OBSTitles.CenterRight"), obsgs_tr("OBSTitles.BottomLeft"), obsgs_tr("OBSTitles.BottomCenter"), obsgs_tr("OBSTitles.BottomRight")})
        cmb_anchor_->addItem(label);
    cmb_anchor_->setToolTip(obsgs_tr("OBSTitles.AnchorChangeTooltip"));
    cmb_anchor_->setFixedHeight(22);
    cmb_anchor_->setStyleSheet(control_style);

    btn_kf_pos_x_ = mk_kf_button(obsgs_tr("OBSTitles.ToggleXKeyframe"));
    btn_kf_pos_y_ = mk_kf_button(obsgs_tr("OBSTitles.ToggleYKeyframe"));
    btn_kf_scale_x_ = mk_kf_button(obsgs_tr("OBSTitles.ToggleScaleXKeyframe"));
    btn_kf_scale_y_ = mk_kf_button(obsgs_tr("OBSTitles.ToggleScaleYKeyframe"));
    btn_kf_rotation_ = mk_kf_button(obsgs_tr("OBSTitles.ToggleRotationKeyframe"));
    btn_kf_opacity_ = mk_kf_button(obsgs_tr("OBSTitles.ToggleOpacityKeyframe"));
    btn_kf_origin_x_ = mk_kf_button(obsgs_tr("OBSTitles.ToggleOriginXKeyframe"));
    btn_kf_origin_y_ = mk_kf_button(obsgs_tr("OBSTitles.ToggleOriginYKeyframe"));
    add_form_row(tfl, obsgs_tr("OBSTitles.XLabel"),       with_kf(spn_px_, btn_kf_pos_x_));
    add_form_row(tfl, obsgs_tr("OBSTitles.YLabel"),       with_kf(spn_py_, btn_kf_pos_y_));
    add_form_row(tfl, obsgs_tr("OBSTitles.ScaleXLabel"),  with_kf(spn_scale_x_, btn_kf_scale_x_));
    add_form_row(tfl, obsgs_tr("OBSTitles.ScaleYLabel"),  with_kf(spn_scale_y_, btn_kf_scale_y_));
    add_form_row(tfl, QString(), chk_scale_lock_);
    add_form_row(tfl, obsgs_tr("OBSTitles.RotationLabel"),with_kf(spn_rot_, btn_kf_rotation_));
    add_form_row(tfl, obsgs_tr("OBSTitles.OpacityLabel"), with_kf(spn_opacity_, btn_kf_opacity_));
    add_form_row(tfl, obsgs_tr("OBSTitles.AnchorLabel"), with_kf(cmb_anchor_, mk_kf_button(obsgs_tr("OBSTitles.ToggleAnchorKeyframe"))));
    add_form_row(tfl, obsgs_tr("OBSTitles.OriginXLabel"), with_kf(spn_origin_x_, btn_kf_origin_x_));
    add_form_row(tfl, obsgs_tr("OBSTitles.OriginYLabel"), with_kf(spn_origin_y_, btn_kf_origin_y_));
    vl->addWidget(tform_box);
    make_collapsible(tform_box);

    auto make_property_grid = [&](QWidget *parent_widget) {
        auto *grid = new QGridLayout(parent_widget);
        grid->setContentsMargins(6, 5, 6, 6);
        grid->setHorizontalSpacing(5);
        grid->setVerticalSpacing(3);
        grid->setColumnStretch(1, 1);
        grid->setColumnStretch(3, 1);
        return grid;
    };
    auto grid_label = [&](const QString &text, QWidget *parent_widget, QWidget *field = nullptr) {
        auto *label = new NumericDragLabel(text, field, parent_widget,
                                           [this]() {
                                               if (loading_values_) return;
                                               numeric_label_dragging_ = true;
                                               emit property_changed(true);
                                           },
                                           [this]() {
                                               if (loading_values_) return;
                                               numeric_label_dragging_ = false;
                                               emit property_changed(true);
                                           });
        label->setStyleSheet("color:#9f9f9f;font-size:10px;");
        label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        return label;
    };
    auto add_grid_field = [&](QGridLayout *grid, int row, int col, const QString &label_text, QWidget *field) {
        QWidget *parent_widget = grid->parentWidget();
        grid->addWidget(grid_label(label_text, parent_widget, field), row, col * 2);
        grid->addWidget(field, row, col * 2 + 1);
    };
    auto add_full_width_field = [&](QGridLayout *grid, int row, const QString &label_text, QWidget *field) {
        QWidget *parent_widget = grid->parentWidget();
        grid->addWidget(grid_label(label_text, parent_widget, field), row, 0);
        grid->addWidget(field, row, 1, 1, 3);
    };
    auto mk_combo = [&](const QStringList &labels, const QList<int> &values) {
        auto *combo = new QComboBox(inner);
        for (int i = 0; i < labels.size(); ++i)
            combo->addItem(labels[i], i < values.size() ? values[i] : i);
        combo->setFixedHeight(22);
        combo->setStyleSheet(control_style);
        return combo;
    };
    auto mk_type_button = [&](const QString &label, const QString &tip) {
        auto *button = new QToolButton(inner);
        button->setText(label);
        button->setToolTip(tip);
        button->setCheckable(true);
        button->setFixedSize(28, 22);
        button->setAutoRaise(false);
        button->setStyleSheet(
            "QToolButton{color:#d8d8d8;background:#242424;border:1px solid #373737;border-radius:2px;"
            "font-size:10px;font-weight:bold;padding:0;}"
            "QToolButton:hover{background:#303030;border-color:#4a4a4a;}"
            "QToolButton:checked{background:#4b6ea8;color:white;border-color:#6f8fc4;}");
        return button;
    };

    /* ── Character ── */
    text_box_ = new QGroupBox("Character", inner);
    text_box_->setStyleSheet(section_style);
    auto *char_grid = make_property_grid(text_box_);

    txt_content_ = new QTextEdit(inner);
    txt_content_->setAcceptRichText(false);
    txt_content_->setMinimumHeight(72);
    txt_content_->setMaximumHeight(92);
    txt_content_->setPlaceholderText(obsgs_tr("OBSTitles.EnterTextPlaceholder"));
    txt_content_->setStyleSheet(control_style);

    cmb_font_ = new QComboBox(inner);
    cmb_font_->setFixedHeight(22);
    cmb_font_->setEditable(true);
    cmb_font_->setInsertPolicy(QComboBox::NoInsert);
    cmb_font_->setMaxVisibleItems(24);
    cmb_font_->setStyleSheet(control_style);
    QFontDatabase fdb;
    for (auto &fam : fdb.families())
        cmb_font_->addItem(fam, fam);

    cmb_font_style_ = new QComboBox(inner);
    cmb_font_style_->setFixedHeight(22);
    cmb_font_style_->setStyleSheet(control_style);

    spn_size_ = new QSpinBox(inner);
    spn_size_->setRange(6, 500);
    spn_size_->setFixedHeight(22);
    spn_size_->setStyleSheet(control_style);

    cmb_kerning_mode_ = mk_combo({"Metrics", "Optical", "Manual"}, {0, 1, 2});
    spn_kerning_value_ = mk_dspin(-100.0, 500.0, 1.0);
    spn_kerning_value_->setSuffix(" px");
    spn_kerning_value_->setToolTip("Manual kerning adjustment added to tracking.");
    spn_text_leading_ = mk_dspin(-200.0, 500.0, 1.0);
    spn_text_leading_->setSuffix(" px");
    spn_text_leading_->setToolTip(obsgs_tr("OBSTitles.LeadingTooltip"));
    spn_char_tracking_ = mk_dspin(-100.0, 500.0, 1.0);
    spn_char_tracking_->setSuffix(" px");
    spn_char_tracking_->setToolTip(obsgs_tr("OBSTitles.TrackingTooltip"));
    spn_char_scale_x_ = mk_dspin(10.0, 500.0, 1.0);
    spn_char_scale_x_->setSuffix("%");
    spn_char_scale_y_ = mk_dspin(10.0, 500.0, 1.0);
    spn_char_scale_y_->setSuffix("%");
    spn_baseline_shift_ = mk_dspin(-500.0, 500.0, 1.0);
    spn_baseline_shift_->setSuffix(" px");
    cmb_language_ = mk_combo({"English", "Arabic", "Chinese", "French", "German", "Japanese", "Korean", "Portuguese", "Spanish"}, {});
    btn_text_color_ = new QPushButton(inner);
    btn_text_color_->setFixedHeight(22);
    btn_kf_text_color_ = mk_kf_button(obsgs_tr("OBSTitles.ToggleTextColorKeyframe"));

    char_grid->addWidget(grid_label(obsgs_tr("OBSTitles.TextLabel"), text_box_), 0, 0);
    char_grid->addWidget(txt_content_, 0, 1, 1, 3);
    add_full_width_field(char_grid, 1, "Font", cmb_font_);
    add_full_width_field(char_grid, 2, "Style", cmb_font_style_);
    add_grid_field(char_grid, 3, 0, "Size", spn_size_);
    add_grid_field(char_grid, 3, 1, "Leading", spn_text_leading_);
    add_grid_field(char_grid, 4, 0, "Kerning", cmb_kerning_mode_);
    add_grid_field(char_grid, 4, 1, "Value", spn_kerning_value_);
    add_grid_field(char_grid, 5, 0, "H Scale", spn_char_scale_x_);
    add_grid_field(char_grid, 5, 1, "V Scale", spn_char_scale_y_);
    add_grid_field(char_grid, 6, 0, "Tracking", spn_char_tracking_);
    add_grid_field(char_grid, 6, 1, "Baseline", spn_baseline_shift_);
    add_full_width_field(char_grid, 7, "Fill Color", with_kf(btn_text_color_, btn_kf_text_color_));
    add_grid_field(char_grid, 8, 0, "Language", cmb_language_);
    vl->addWidget(text_box_);
    make_collapsible(text_box_);

    /* ── Type Options ── */
    type_options_box_ = new QGroupBox("Type Options", inner);
    type_options_box_->setStyleSheet(section_style);
    auto *type_grid = new QGridLayout(type_options_box_);
    type_grid->setContentsMargins(6, 5, 6, 6);
    type_grid->setHorizontalSpacing(4);
    type_grid->setVerticalSpacing(4);
    chk_bold_ = mk_type_button("B", obsgs_tr("OBSTitles.Bold"));
    chk_italic_ = mk_type_button("I", obsgs_tr("OBSTitles.Italic"));
    chk_font_kerning_ = mk_type_button("K", obsgs_tr("OBSTitles.Kerning"));
    chk_font_kerning_->setToolTip(obsgs_tr("OBSTitles.KerningTooltip"));
    btn_all_caps_ = mk_type_button("TT", "All Caps");
    btn_small_caps_ = mk_type_button("Tᴛ", "Small Caps");
    btn_superscript_ = mk_type_button("x²", "Superscript");
    btn_subscript_ = mk_type_button("x₂", "Subscript");
    btn_underline_ = mk_type_button("U", "Underline");
    btn_strikethrough_ = mk_type_button("S", "Strikethrough");
    btn_ligatures_ = mk_type_button("fi", "Ligatures");
    btn_stylistic_alternates_ = mk_type_button("Sw", "Stylistic Alternates");
    btn_fractions_ = mk_type_button("½", "Fractions");
    btn_opentype_features_ = mk_type_button("OT", "OpenType Features");
    QList<QToolButton *> type_buttons{chk_bold_, chk_italic_, btn_all_caps_, btn_small_caps_, btn_superscript_,
                                      btn_subscript_, btn_underline_, btn_strikethrough_, btn_ligatures_, btn_stylistic_alternates_,
                                      btn_fractions_, btn_opentype_features_, chk_font_kerning_};
    for (int i = 0; i < type_buttons.size(); ++i) type_grid->addWidget(type_buttons[i], i / 5, i % 5);
    type_grid->setColumnStretch(5, 1);
    vl->addWidget(type_options_box_);
    make_collapsible(type_options_box_);

    /* ── Paragraph ── */
    paragraph_box_ = new QGroupBox("Paragraph", inner);
    paragraph_box_->setStyleSheet(section_style);
    auto *paragraph_layout = new QVBoxLayout(paragraph_box_);
    paragraph_layout->setContentsMargins(6, 5, 6, 6);
    paragraph_layout->setSpacing(5);
    auto *align_row = new QWidget(inner);
    auto *align_layout = new QHBoxLayout(align_row);
    align_layout->setContentsMargins(0, 0, 0, 0);
    align_layout->setSpacing(4);
    cmb_text_align_ = new QComboBox(inner);
    cmb_text_align_->addItem(obsgs_tr("OBSTitles.AlignLeft"), 0);
    cmb_text_align_->addItem(obsgs_tr("OBSTitles.AlignCenter"), 1);
    cmb_text_align_->addItem(obsgs_tr("OBSTitles.AlignRight"), 2);
    cmb_text_align_->addItem("Justify Left", 0);
    cmb_text_align_->addItem("Justify Center", 1);
    cmb_text_align_->addItem("Justify Right", 2);
    cmb_text_align_->addItem("Full Justify", 1);
    cmb_text_align_->setFixedHeight(22);
    cmb_text_align_->setStyleSheet(control_style);
    align_layout->addWidget(cmb_text_align_, 1);
    cmb_text_valign_ = new QComboBox(inner);
    cmb_text_valign_->addItem(obsgs_tr("OBSTitles.AlignTop"), 0);
    cmb_text_valign_->addItem(obsgs_tr("OBSTitles.AlignMiddle"), 1);
    cmb_text_valign_->addItem(obsgs_tr("OBSTitles.AlignBottom"), 2);
    cmb_text_valign_->setFixedHeight(22);
    cmb_text_valign_->setStyleSheet(control_style);
    align_layout->addWidget(cmb_text_valign_, 1);
    paragraph_layout->addWidget(align_row);
    vl->addWidget(paragraph_box_);
    make_collapsible(paragraph_box_);

    /* ── Dynamic Text ── */
    dynamic_text_box_ = new QGroupBox("Dynamic Text", inner);
    dynamic_text_box_->setStyleSheet(section_style);
    auto *dynamic_form = new QFormLayout(dynamic_text_box_);
    style_form(dynamic_form);
    cmb_text_style_ = new QComboBox(inner);
    cmb_text_style_->addItem(obsgs_tr("OBSTitles.Normal"), 0);
    cmb_text_style_->addItem(obsgs_tr("OBSTitles.AllCaps"), 1);
    cmb_text_style_->addItem(obsgs_tr("OBSTitles.SmallCaps"), 2);
    cmb_text_style_->addItem(obsgs_tr("OBSTitles.Superscript"), 3);
    cmb_text_style_->addItem(obsgs_tr("OBSTitles.Subscript"), 4);
    cmb_text_style_->setToolTip(obsgs_tr("OBSTitles.TextStyleTooltip"));
    cmb_text_style_->setFixedHeight(22);
    cmb_text_style_->setStyleSheet(control_style);
    cmb_text_overflow_ = new QComboBox(inner);
    cmb_text_overflow_->addItem(obsgs_tr("OBSTitles.Wrap"), 0);
    cmb_text_overflow_->addItem(obsgs_tr("OBSTitles.Clip"), 1);
    cmb_text_overflow_->addItem(obsgs_tr("OBSTitles.HorizontalFit"), 2);
    cmb_text_overflow_->setToolTip(obsgs_tr("OBSTitles.TextOverflowTooltip"));
    cmb_text_overflow_->setFixedHeight(22);
    cmb_text_overflow_->setStyleSheet(control_style);
    spn_text_fit_min_scale_ = mk_dspin(0.05, 1.0, 0.05);
    spn_text_fit_min_scale_->setDecimals(2);
    spn_text_fit_min_scale_->setToolTip(obsgs_tr("OBSTitles.MinFitScaleTooltip"));
    lbl_text_fit_scale_ = new QLabel(obsgs_tr("OBSTitles.Scale100"), inner);
    lbl_text_fit_scale_->setStyleSheet("color:#999;font-size:10px;");
    chk_expose_text_ = new QCheckBox(obsgs_tr("OBSTitles.ExposeInDock"), inner);
    chk_expose_text_->setToolTip(obsgs_tr("OBSTitles.ExposeInDockTooltip"));
    style_checkbox(chk_expose_text_);
    cmb_ticker_style_ = new QComboBox(inner);
    cmb_ticker_style_->addItem(obsgs_tr("OBSTitles.TickerHorizontal"), 0);
    cmb_ticker_style_->addItem(obsgs_tr("OBSTitles.TickerVerticalLine"), 1);
    cmb_ticker_style_->addItem(obsgs_tr("OBSTitles.TickerVerticalSmooth"), 2);
    cmb_ticker_style_->setFixedHeight(22);
    cmb_ticker_style_->setStyleSheet(control_style);
    spn_ticker_speed_ = mk_dspin(1.0, 5000.0, 1.0);
    spn_ticker_speed_->setSuffix(" px/s");
    spn_ticker_line_hold_ = mk_dspin(0.1, 60.0, 0.1);
    spn_ticker_line_hold_->setSuffix(" s");
    cmb_ticker_direction_ = new QComboBox(inner);
    cmb_ticker_direction_->setFixedHeight(22);
    cmb_ticker_direction_->setStyleSheet(control_style);
    add_form_row(dynamic_form, "Text Style", cmb_text_style_);
    add_form_row(dynamic_form, obsgs_tr("OBSTitles.OverflowLabel"), cmb_text_overflow_);
    add_form_row(dynamic_form, obsgs_tr("OBSTitles.MinFitScaleLabel"), spn_text_fit_min_scale_);
    add_form_row(dynamic_form, "", lbl_text_fit_scale_);
    add_form_row(dynamic_form, obsgs_tr("OBSTitles.LiveEditLabel"), chk_expose_text_);
    add_form_row(dynamic_form, obsgs_tr("OBSTitles.TickerStyleLabel"), cmb_ticker_style_);
    add_form_row(dynamic_form, obsgs_tr("OBSTitles.TickerSpeedLabel"), spn_ticker_speed_);
    add_form_row(dynamic_form, obsgs_tr("OBSTitles.TickerLineHoldLabel"), spn_ticker_line_hold_);
    add_form_row(dynamic_form, obsgs_tr("OBSTitles.DirectionLabel"), cmb_ticker_direction_);
    vl->addWidget(dynamic_text_box_);
    make_collapsible(dynamic_text_box_);

    /* ── Bullets and Numbering ── */
    bullets_box_ = new QGroupBox("Bullets and Numbering", inner);
    bullets_box_->setStyleSheet(section_style);
    auto *bullets_layout = new QVBoxLayout(bullets_box_);
    bullets_layout->setContentsMargins(6, 5, 6, 6);
    auto *bullets_hint = new QLabel("Broadcast lower thirds typically use manual bullet glyphs; this group is ready for list presets.", inner);
    bullets_hint->setWordWrap(true);
    bullets_hint->setStyleSheet("color:#8f8f8f;font-size:10px;");
    bullets_layout->addWidget(bullets_hint);
    vl->addWidget(bullets_box_);
    make_collapsible(bullets_box_);

    /* ── Rectangle ── */
    rect_box_ = new QGroupBox(obsgs_tr("OBSTitles.Rectangle"), inner);
    rect_box_->setStyleSheet(section_style);
    auto *rfl = new QFormLayout(rect_box_);
    style_form(rfl);
    spn_layer_w_ = mk_dspin(0.0, 9999.0, 10.0);
    spn_layer_h_ = mk_dspin(0.0, 9999.0, 10.0);
    chk_text_box_width_to_text_ = new QCheckBox(obsgs_tr("OBSTitles.TextBoxWidthToText"), inner);
    chk_text_box_height_to_text_ = new QCheckBox(obsgs_tr("OBSTitles.TextBoxHeightToText"), inner);
    style_checkbox(chk_text_box_width_to_text_);
    style_checkbox(chk_text_box_height_to_text_);
    spn_max_text_box_width_ = mk_dspin(1.0, 9999.0, 10.0);
    spn_max_text_box_height_ = mk_dspin(1.0, 9999.0, 10.0);
    spn_rect_corner_ = mk_dspin(0.0, 1000.0, 1.0);
    btn_kf_width_ = mk_kf_button(obsgs_tr("OBSTitles.ToggleWidthKeyframe"));
    btn_kf_height_ = mk_kf_button(obsgs_tr("OBSTitles.ToggleHeightKeyframe"));
    add_form_row(rfl, obsgs_tr("OBSTitles.WidthLabel"), with_kf(spn_layer_w_, btn_kf_width_));
    add_form_row(rfl, "", chk_text_box_width_to_text_);
    add_form_row(rfl, obsgs_tr("OBSTitles.MaxTextBoxWidthLabel"), spn_max_text_box_width_);
    add_form_row(rfl, obsgs_tr("OBSTitles.HeightLabel"), with_kf(spn_layer_h_, btn_kf_height_));
    add_form_row(rfl, "", chk_text_box_height_to_text_);
    add_form_row(rfl, obsgs_tr("OBSTitles.MaxTextBoxHeightLabel"), spn_max_text_box_height_);
    add_form_row(rfl, obsgs_tr("OBSTitles.CornerLabel"), with_kf(spn_rect_corner_, mk_kf_button(obsgs_tr("OBSTitles.ToggleCornerKeyframe"))));
    cmb_fill_type_ = new QComboBox(inner);
    cmb_fill_type_->addItem(obsgs_tr("OBSTitles.Solid"), 0);
    cmb_fill_type_->addItem(obsgs_tr("OBSTitles.Gradient"), 1);
    cmb_fill_type_->setFixedHeight(22);
    cmb_fill_type_->setStyleSheet(control_style);
    row_fill_type_ = cmb_fill_type_;
    add_form_row(rfl, obsgs_tr("OBSTitles.FillTypeLabel"), row_fill_type_);
    btn_fill_color_ = new QPushButton(inner);
    btn_kf_fill_color_ = mk_kf_button(obsgs_tr("OBSTitles.ToggleFillColorKeyframe"));
    row_fill_color_ = with_kf(btn_fill_color_, btn_kf_fill_color_);
    add_form_row(rfl, obsgs_tr("OBSTitles.ColorLabel"), row_fill_color_);
    chk_background_enabled_ = new QCheckBox(obsgs_tr("OBSTitles.EnableColorBackground"), inner);
    style_checkbox(chk_background_enabled_);
    btn_background_color_ = new QPushButton(inner);
    spn_background_opacity_ = mk_dspin(0.0, 1.0, 0.05);
    spn_background_opacity_->setDecimals(2);
    spn_background_padding_x_ = mk_dspin(0.0, 1000.0, 1.0);
    spn_background_padding_y_ = mk_dspin(0.0, 1000.0, 1.0);
    spn_background_corner_ = mk_dspin(0.0, 1000.0, 1.0);
    btn_kf_background_enabled_ = mk_kf_button(obsgs_tr("OBSTitles.ToggleBackgroundEnabledKeyframe"));
    btn_kf_background_color_ = mk_kf_button(obsgs_tr("OBSTitles.ToggleBackgroundColorKeyframe"));
    btn_kf_background_opacity_ = mk_kf_button(obsgs_tr("OBSTitles.ToggleBackgroundOpacityKeyframe"));
    btn_kf_background_padding_x_ = mk_kf_button(obsgs_tr("OBSTitles.ToggleBackgroundHorizontalPaddingKeyframe"));
    btn_kf_background_padding_y_ = mk_kf_button(obsgs_tr("OBSTitles.ToggleBackgroundVerticalPaddingKeyframe"));
    btn_kf_background_corner_ = mk_kf_button(obsgs_tr("OBSTitles.ToggleBackgroundCornerKeyframe"));
    row_background_enabled_ = with_kf(chk_background_enabled_, btn_kf_background_enabled_);
    row_background_color_ = with_kf(btn_background_color_, btn_kf_background_color_);
    row_background_opacity_ = with_kf(spn_background_opacity_, btn_kf_background_opacity_);
    row_background_padding_x_ = with_kf(spn_background_padding_x_, btn_kf_background_padding_x_);
    row_background_padding_y_ = with_kf(spn_background_padding_y_, btn_kf_background_padding_y_);
    row_background_corner_ = with_kf(spn_background_corner_, btn_kf_background_corner_);
    add_form_row(rfl, "", row_background_enabled_);
    add_form_row(rfl, obsgs_tr("OBSTitles.BackgroundColorLabel"), row_background_color_);
    add_form_row(rfl, obsgs_tr("OBSTitles.BackgroundOpacityLabel"), row_background_opacity_);
    add_form_row(rfl, obsgs_tr("OBSTitles.BackgroundHorizontalPaddingLabel"), row_background_padding_x_);
    add_form_row(rfl, obsgs_tr("OBSTitles.BackgroundVerticalPaddingLabel"), row_background_padding_y_);
    add_form_row(rfl, obsgs_tr("OBSTitles.BackgroundCornerLabel"), row_background_corner_);
    spn_outline_width_ = mk_dspin(0.0, 200.0, 1.0);
    spn_outline_width_->setToolTip(obsgs_tr("OBSTitles.OutlineWidthTooltip"));
    btn_outline_color_ = new QPushButton(inner);
    row_outline_color_ = btn_outline_color_;
    add_form_row(rfl, obsgs_tr("OBSTitles.OutlineWidthLabel"), spn_outline_width_);
    add_form_row(rfl, obsgs_tr("OBSTitles.OutlineColorLabel"), row_outline_color_);
    vl->addWidget(rect_box_);
    make_collapsible(rect_box_);

    /* ── Gradient Properties ── */
    gradient_box_ = new QGroupBox(obsgs_tr("OBSTitles.GradientProperties"), inner);
    gradient_box_->setStyleSheet(section_style);
    auto *gfl = new QFormLayout(gradient_box_);
    style_form(gfl);
    cmb_gradient_type_ = new QComboBox(inner);
    cmb_gradient_type_->addItem(obsgs_tr("OBSTitles.LinearGradient"), 0);
    cmb_gradient_type_->addItem(obsgs_tr("OBSTitles.RadialGradient"), 1);
    cmb_gradient_type_->setFixedHeight(22);
    cmb_gradient_type_->setStyleSheet(control_style);
    btn_gradient_start_color_ = new QPushButton(inner);
    btn_gradient_end_color_ = new QPushButton(inner);
    spn_gradient_start_pos_ = mk_dspin(0.0, 1.0, 0.01);
    spn_gradient_end_pos_ = mk_dspin(0.0, 1.0, 0.01);
    spn_gradient_start_opacity_ = mk_dspin(0.0, 1.0, 0.01);
    spn_gradient_end_opacity_ = mk_dspin(0.0, 1.0, 0.01);
    spn_gradient_opacity_ = mk_dspin(0.0, 1.0, 0.01);
    spn_gradient_angle_ = mk_dspin(-360.0, 360.0, 1.0);
    spn_gradient_center_x_ = mk_dspin(0.0, 1.0, 0.01);
    spn_gradient_center_y_ = mk_dspin(0.0, 1.0, 0.01);
    spn_gradient_scale_ = mk_dspin(0.01, 10.0, 0.05);
    spn_gradient_focal_x_ = mk_dspin(0.0, 1.0, 0.01);
    spn_gradient_focal_y_ = mk_dspin(0.0, 1.0, 0.01);
    for (auto *spin : std::initializer_list<QDoubleSpinBox *>{spn_gradient_start_pos_, spn_gradient_end_pos_,
                                                               spn_gradient_start_opacity_, spn_gradient_end_opacity_,
                                                               spn_gradient_opacity_, spn_gradient_center_x_,
                                                               spn_gradient_center_y_, spn_gradient_scale_,
                                                               spn_gradient_focal_x_, spn_gradient_focal_y_})
        spin->setDecimals(2);
    spn_gradient_angle_->setSuffix("°");
    add_form_row(gfl, obsgs_tr("OBSTitles.GradientTypeLabel"), cmb_gradient_type_);
    add_form_row(gfl, obsgs_tr("OBSTitles.StartColorLabel"), btn_gradient_start_color_);
    add_form_row(gfl, obsgs_tr("OBSTitles.StartStopLabel"), spn_gradient_start_pos_);
    add_form_row(gfl, obsgs_tr("OBSTitles.StartOpacityLabel"), spn_gradient_start_opacity_);
    add_form_row(gfl, obsgs_tr("OBSTitles.EndColorLabel"), btn_gradient_end_color_);
    add_form_row(gfl, obsgs_tr("OBSTitles.EndStopLabel"), spn_gradient_end_pos_);
    add_form_row(gfl, obsgs_tr("OBSTitles.EndOpacityLabel"), spn_gradient_end_opacity_);
    add_form_row(gfl, obsgs_tr("OBSTitles.OpacityLabel"), spn_gradient_opacity_);
    add_form_row(gfl, obsgs_tr("OBSTitles.AngleLabel"), spn_gradient_angle_);
    add_form_row(gfl, obsgs_tr("OBSTitles.CenterXLabel"), spn_gradient_center_x_);
    add_form_row(gfl, obsgs_tr("OBSTitles.CenterYLabel"), spn_gradient_center_y_);
    add_form_row(gfl, obsgs_tr("OBSTitles.ScaleLabel"), spn_gradient_scale_);
    add_form_row(gfl, obsgs_tr("OBSTitles.FocalXLabel"), spn_gradient_focal_x_);
    add_form_row(gfl, obsgs_tr("OBSTitles.FocalYLabel"), spn_gradient_focal_y_);
    vl->addWidget(gradient_box_);
    make_collapsible(gradient_box_);

    /* ── Outline ── */
    outline_box_ = new QGroupBox(obsgs_tr("OBSTitles.Outline"), inner);
    outline_box_->setStyleSheet(section_style);
    auto *outline_form = new QFormLayout(outline_box_);
    style_form(outline_form);
    chk_outline_enabled_ = new QCheckBox(obsgs_tr("OBSTitles.EnableOutline"), inner);
    style_checkbox(chk_outline_enabled_);
    spn_outline_width_ = mk_dspin(0.0, 200.0, 1.0);
    spn_outline_width_->setToolTip(obsgs_tr("OBSTitles.OutlineThicknessTooltip"));
    btn_outline_color_ = new QPushButton(inner);
    row_outline_color_ = btn_outline_color_;
    spn_outline_opacity_ = mk_dspin(0.0, 1.0, 0.05);
    spn_outline_opacity_->setDecimals(2);
    cmb_outline_join_ = new QComboBox(inner);
    cmb_outline_join_->addItem(obsgs_tr("OBSTitles.Miter"), 0);
    cmb_outline_join_->addItem(obsgs_tr("OBSTitles.Round"), 1);
    cmb_outline_join_->addItem(obsgs_tr("OBSTitles.Bevel"), 2);
    cmb_outline_join_->setFixedHeight(22);
    cmb_outline_join_->setStyleSheet(control_style);
    cmb_outline_position_ = new QComboBox(inner);
    cmb_outline_position_->addItem(obsgs_tr("OBSTitles.Back"), 0);
    cmb_outline_position_->addItem(obsgs_tr("OBSTitles.Front"), 1);
    cmb_outline_position_->setFixedHeight(22);
    cmb_outline_position_->setStyleSheet(control_style);
    chk_outline_antialias_ = new QCheckBox(obsgs_tr("OBSTitles.AntialiasOutline"), inner);
    style_checkbox(chk_outline_antialias_);
    add_form_row(outline_form, "", chk_outline_enabled_);
    add_form_row(outline_form, obsgs_tr("OBSTitles.ColorLabel"), btn_outline_color_);
    add_form_row(outline_form, obsgs_tr("OBSTitles.ThicknessLabel"), spn_outline_width_);
    add_form_row(outline_form, obsgs_tr("OBSTitles.OpacityLabel"), spn_outline_opacity_);
    add_form_row(outline_form, obsgs_tr("OBSTitles.JoinLabel"), cmb_outline_join_);
    add_form_row(outline_form, obsgs_tr("OBSTitles.PositionLabelIndented"), cmb_outline_position_);
    add_form_row(outline_form, "", chk_outline_antialias_);
    vl->addWidget(outline_box_);
    make_collapsible(outline_box_);

    /* ── Image ── */
    image_box_ = new QGroupBox(obsgs_tr("OBSTitles.Image"), inner);
    image_box_->setStyleSheet(section_style);
    auto *image_form = new QFormLayout(image_box_);
    style_form(image_form);
    edit_image_path_ = new QLineEdit(inner);
    edit_image_path_->setFixedHeight(22);
    edit_image_path_->setStyleSheet(control_style);
    btn_pick_image_ = new QPushButton(obsgs_tr("OBSTitles.Browse"), inner);
    style_push_button(btn_pick_image_);
    spn_layer_w_->setToolTip(obsgs_tr("OBSTitles.ImageWidthTooltip"));
    spn_layer_h_->setToolTip(obsgs_tr("OBSTitles.ImageHeightTooltip"));
    chk_lock_aspect_ = new QCheckBox(obsgs_tr("OBSTitles.LockAspectRatio"), inner);
    style_checkbox(chk_lock_aspect_);
    add_form_row(image_form, obsgs_tr("OBSTitles.PathLabel"), edit_image_path_);
    add_form_row(image_form, "", btn_pick_image_);
    add_form_row(image_form, "", chk_lock_aspect_);
    vl->addWidget(image_box_);
    make_collapsible(image_box_);

    shadow_box_ = new QGroupBox(obsgs_tr("OBSTitles.DropShadow"), inner);
    shadow_box_->setStyleSheet(section_style);
    auto *sfl = new QFormLayout(shadow_box_);
    style_form(sfl);
    chk_shadow_enabled_ = new QCheckBox(obsgs_tr("OBSTitles.EnableShadow"), inner);
    style_checkbox(chk_shadow_enabled_);
    cmb_shadow_preset_ = new QComboBox(inner);
    cmb_shadow_preset_->addItems({obsgs_tr("OBSTitles.Custom"), obsgs_tr("OBSTitles.Soft"), obsgs_tr("OBSTitles.Medium"), obsgs_tr("OBSTitles.Strong"), obsgs_tr("OBSTitles.Broadcast")});
    cmb_shadow_preset_->setFixedHeight(22);
    cmb_shadow_preset_->setStyleSheet(control_style);
    btn_shadow_color_ = new QPushButton(inner);
    spn_shadow_opacity_ = mk_dspin(0.0, 1.0, 0.05);
    spn_shadow_opacity_->setDecimals(2);
    spn_shadow_distance_ = mk_dspin(0.0, 200.0, 1.0);
    spn_shadow_angle_ = mk_dspin(-360.0, 360.0, 5.0);
    spn_shadow_blur_ = mk_dspin(0.0, 100.0, 1.0);
    spn_shadow_spread_ = mk_dspin(0.0, 100.0, 1.0);
    btn_kf_shadow_enabled_ = mk_kf_button(obsgs_tr("OBSTitles.ToggleShadowEnabledKeyframe"));
    btn_kf_shadow_color_ = mk_kf_button(obsgs_tr("OBSTitles.ToggleShadowColorKeyframe"));
    btn_kf_shadow_opacity_ = mk_kf_button(obsgs_tr("OBSTitles.ToggleShadowOpacityKeyframe"));
    btn_kf_shadow_distance_ = mk_kf_button(obsgs_tr("OBSTitles.ToggleShadowDistanceKeyframe"));
    btn_kf_shadow_angle_ = mk_kf_button(obsgs_tr("OBSTitles.ToggleShadowAngleKeyframe"));
    btn_kf_shadow_blur_ = mk_kf_button(obsgs_tr("OBSTitles.ToggleShadowBlurKeyframe"));
    btn_kf_shadow_spread_ = mk_kf_button(obsgs_tr("OBSTitles.ToggleShadowSpreadKeyframe"));
    add_form_row(sfl, "", with_kf(chk_shadow_enabled_, btn_kf_shadow_enabled_));
    add_form_row(sfl, obsgs_tr("OBSTitles.PresetLabel"), cmb_shadow_preset_);
    add_form_row(sfl, obsgs_tr("OBSTitles.ColorLabel"), with_kf(btn_shadow_color_, btn_kf_shadow_color_));
    add_form_row(sfl, obsgs_tr("OBSTitles.OpacityLabel"), with_kf(spn_shadow_opacity_, btn_kf_shadow_opacity_));
    add_form_row(sfl, obsgs_tr("OBSTitles.DistanceLabel"), with_kf(spn_shadow_distance_, btn_kf_shadow_distance_));
    add_form_row(sfl, obsgs_tr("OBSTitles.AngleLabel"), with_kf(spn_shadow_angle_, btn_kf_shadow_angle_));
    add_form_row(sfl, obsgs_tr("OBSTitles.BlurLabel"), with_kf(spn_shadow_blur_, btn_kf_shadow_blur_));
    add_form_row(sfl, obsgs_tr("OBSTitles.SpreadLabel"), with_kf(spn_shadow_spread_, btn_kf_shadow_spread_));
    vl->addWidget(shadow_box_);
    make_collapsible(shadow_box_);

    vl->addStretch();
    setWidget(inner);

    /* ── Connect signals → property_changed ── */
    auto emit_change = [this]() { if (!loading_values_) emit property_changed(!numeric_label_dragging_); };
    auto can_edit = [this]() { return layer_ && !loading_values_; };
    auto local_time = [this]() {
        return layer_ ? std::clamp(playhead_ - layer_->in_time, 0.0,
                                   std::max(0.0, layer_->out_time - layer_->in_time)) : 0.0;
    };
    auto update_text_box_auto_controls = [this]() {
        if (spn_max_text_box_width_)
            spn_max_text_box_width_->setEnabled(chk_text_box_width_to_text_ && chk_text_box_width_to_text_->isChecked());
        if (spn_max_text_box_height_)
            spn_max_text_box_height_->setEnabled(chk_text_box_height_to_text_ && chk_text_box_height_to_text_->isChecked());
    };
    auto install_delete_all_keyframes_menu =
        [this, can_edit, emit_change](QPushButton *button, auto props_for_layer) {
            if (!button) return;
            button->setContextMenuPolicy(Qt::CustomContextMenu);
            connect(button, &QPushButton::customContextMenuRequested,
                    this, [this, button, props_for_layer, can_edit, emit_change](const QPoint &pos) {
                        if (!layer_) return;
                        std::vector<AnimatedProperty *> props = props_for_layer();
                        bool has_keyframes = false;
                        for (auto *prop : props) {
                            if (prop && prop->is_animated()) {
                                has_keyframes = true;
                                break;
                            }
                        }

                        QMenu menu(button);
                        menu.setStyleSheet("QMenu{color:#ddd;background:#252525;border:1px solid #3a3a3a;}"
                                           "QMenu::item{padding:5px 22px;}"
                                           "QMenu::item:selected{background:#3b4f64;}"
                                           "QMenu::item:disabled{color:#666;}");
                        QAction *delete_all = menu.addAction(obsgs_tr("OBSTitles.DeleteAllKeyframes"));
                        delete_all->setEnabled(can_edit() && has_keyframes);
                        if (menu.exec(button->mapToGlobal(pos)) != delete_all || !can_edit()) return;

                        bool changed = false;
                        for (auto *prop : props) {
                            if (!prop || prop->keyframes.empty()) continue;
                            prop->keyframes.clear();
                            changed = true;
                        }
                        if (!changed) return;
                        load_values();
                        emit_change();
                    });
        };
    auto install_prop_delete_all = [&](QPushButton *button, AnimatedProperty Layer::*prop) {
        install_delete_all_keyframes_menu(button, [this, prop]() {
            return layer_ ? std::vector<AnimatedProperty *>{&(layer_.get()->*prop)}
                          : std::vector<AnimatedProperty *>{};
        });
    };
    auto install_group_delete_all = [&](QPushButton *button, std::initializer_list<AnimatedProperty Layer::*> props) {
        std::vector<AnimatedProperty Layer::*> prop_members(props);
        install_delete_all_keyframes_menu(button, [this, prop_members]() {
            std::vector<AnimatedProperty *> result;
            if (!layer_) return result;
            result.reserve(prop_members.size());
            for (auto prop : prop_members)
                result.push_back(&(layer_.get()->*prop));
            return result;
        });
    };

    install_prop_delete_all(btn_kf_pos_x_, &Layer::pos_x);
    install_prop_delete_all(btn_kf_pos_y_, &Layer::pos_y);
    install_prop_delete_all(btn_kf_scale_x_, &Layer::scale_x);
    install_prop_delete_all(btn_kf_scale_y_, &Layer::scale_y);
    install_prop_delete_all(btn_kf_rotation_, &Layer::rotation);
    install_prop_delete_all(btn_kf_opacity_, &Layer::opacity);
    install_prop_delete_all(btn_kf_origin_x_, &Layer::origin_x_prop);
    install_prop_delete_all(btn_kf_origin_y_, &Layer::origin_y_prop);
    install_prop_delete_all(btn_kf_width_, &Layer::box_width);
    install_prop_delete_all(btn_kf_height_, &Layer::box_height);
    install_group_delete_all(btn_kf_text_color_, {&Layer::text_color_a, &Layer::text_color_r,
                                                  &Layer::text_color_g, &Layer::text_color_b});
    install_group_delete_all(btn_kf_fill_color_, {&Layer::fill_color_a, &Layer::fill_color_r,
                                                  &Layer::fill_color_g, &Layer::fill_color_b});
    install_prop_delete_all(btn_kf_background_enabled_, &Layer::background_enabled_prop);
    install_group_delete_all(btn_kf_background_color_, {&Layer::background_color_a, &Layer::background_color_r,
                                                        &Layer::background_color_g, &Layer::background_color_b});
    install_prop_delete_all(btn_kf_background_opacity_, &Layer::background_opacity_prop);
    install_prop_delete_all(btn_kf_background_padding_x_, &Layer::background_padding_x_prop);
    install_prop_delete_all(btn_kf_background_padding_y_, &Layer::background_padding_y_prop);
    install_prop_delete_all(btn_kf_background_corner_, &Layer::background_corner_radius_prop);
    install_prop_delete_all(btn_kf_shadow_enabled_, &Layer::shadow_enabled_prop);
    install_group_delete_all(btn_kf_shadow_color_, {&Layer::shadow_color_a, &Layer::shadow_color_r,
                                                    &Layer::shadow_color_g, &Layer::shadow_color_b});
    install_prop_delete_all(btn_kf_shadow_opacity_, &Layer::shadow_opacity_prop);
    install_prop_delete_all(btn_kf_shadow_distance_, &Layer::shadow_distance_prop);
    install_prop_delete_all(btn_kf_shadow_angle_, &Layer::shadow_angle_prop);
    install_prop_delete_all(btn_kf_shadow_blur_, &Layer::shadow_blur_prop);
    install_prop_delete_all(btn_kf_shadow_spread_, &Layer::shadow_spread_prop);

    connect(spn_px_,       QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, local_time, emit_change](double v){
                if (can_edit()) { set_animated_value(layer_->pos_x, local_time(), v); emit_change(); }
            });
    connect(spn_py_,       QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, local_time, emit_change](double v){
                if (can_edit()) { set_animated_value(layer_->pos_y, local_time(), v); emit_change(); }
            });
    connect(spn_scale_x_,  QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, local_time, emit_change](double v){
                if (!can_edit()) return;
                const double scale = v / 100.0;
                const double t = local_time();
                set_animated_value(layer_->scale_x, t, scale);
                if (layer_->scale_lock) {
                    QSignalBlocker blocker(spn_scale_y_);
                    spn_scale_y_->setValue(v);
                    set_animated_value(layer_->scale_y, t, scale);
                }
                emit_change();
            });
    connect(spn_scale_y_,  QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, local_time, emit_change](double v){
                if (!can_edit()) return;
                const double scale = v / 100.0;
                const double t = local_time();
                set_animated_value(layer_->scale_y, t, scale);
                if (layer_->scale_lock) {
                    QSignalBlocker blocker(spn_scale_x_);
                    spn_scale_x_->setValue(v);
                    set_animated_value(layer_->scale_x, t, scale);
                }
                emit_change();
            });
    connect(chk_scale_lock_, &QCheckBox::toggled,
            this, [this, can_edit, local_time, emit_change](bool locked) {
                if (!can_edit()) return;
                layer_->scale_lock = locked;
                if (locked) {
                    const double t = local_time();
                    const double scale = spn_scale_x_->value() / 100.0;
                    QSignalBlocker blocker(spn_scale_y_);
                    spn_scale_y_->setValue(spn_scale_x_->value());
                    set_animated_value(layer_->scale_y, t, scale);
                }
                emit_change();
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
    connect(txt_content_, &QTextEdit::textChanged,
            this, [this, can_edit, emit_change]() {
                if (!can_edit()) return;
                std::string value = txt_content_->toPlainText().toStdString();
                if (layer_->type == LayerType::Clock)
                    layer_->clock_format = value.empty() ? "H:i:s" : value;
                else
                    layer_->text_content = value;
                emit_change();
            });
    connect(chk_text_box_width_to_text_, &QCheckBox::toggled,
            this, [this, can_edit, update_text_box_auto_controls, emit_change](bool v) {
                update_text_box_auto_controls();
                if (can_edit()) { layer_->text_box_width_to_text = v; emit_change(); }
            });
    connect(chk_text_box_height_to_text_, &QCheckBox::toggled,
            this, [this, can_edit, update_text_box_auto_controls, emit_change](bool v) {
                update_text_box_auto_controls();
                if (can_edit()) { layer_->text_box_height_to_text = v; emit_change(); }
            });
    connect(spn_max_text_box_width_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, emit_change](double v) {
                if (can_edit()) { layer_->max_text_box_width = (float)v; emit_change(); }
            });
    connect(spn_max_text_box_height_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, emit_change](double v) {
                if (can_edit()) { layer_->max_text_box_height = (float)v; emit_change(); }
            });
    connect(cmb_font_, &QComboBox::currentTextChanged,
            this, [this, can_edit, emit_change](const QString &s){
                if (!can_edit()) return;
                populate_font_style_combo(cmb_font_style_, s, QString::fromStdString(layer_->font_style));
                layer_->font_family = s.toStdString();
                layer_->font_style = cmb_font_style_->currentText().toStdString();
                QFontDatabase fdb;
                layer_->font_bold = fdb.bold(s, cmb_font_style_->currentText());
                layer_->font_italic = fdb.italic(s, cmb_font_style_->currentText());
                emit_change();
            });
    connect(cmb_font_style_, &QComboBox::currentTextChanged,
            this, [this, can_edit, emit_change](const QString &s){
                if (!can_edit()) return;
                layer_->font_style = s.toStdString();
                QFontDatabase fdb;
                const QString family = QString::fromStdString(layer_->font_family);
                layer_->font_bold = fdb.bold(family, s);
                layer_->font_italic = fdb.italic(family, s);
                emit_change();
            });
    connect(spn_size_, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [this, can_edit, emit_change](int v){
                if (can_edit()) { layer_->font_size = v; emit_change(); }
            });
    connect(chk_bold_, &QToolButton::toggled,
            this, [this, can_edit, emit_change](bool v){
                if (can_edit()) { layer_->font_bold = v; emit_change(); }
            });
    connect(chk_italic_, &QToolButton::toggled,
            this, [this, can_edit, emit_change](bool v){
                if (can_edit()) { layer_->font_italic = v; emit_change(); }
            });
    connect(chk_font_kerning_, &QToolButton::toggled,
            this, [this, can_edit, emit_change](bool v){
                if (can_edit()) { layer_->font_kerning = v; emit_change(); }
            });
    connect(cmb_kerning_mode_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this, can_edit, emit_change](int idx) {
                if (!can_edit()) return;
                layer_->kerning_mode = cmb_kerning_mode_->itemData(idx).toInt();
                layer_->font_kerning = layer_->kerning_mode != 2;
                if (chk_font_kerning_) chk_font_kerning_->setChecked(layer_->font_kerning);
                if (spn_kerning_value_) spn_kerning_value_->setEnabled(layer_->kerning_mode == 2);
                emit_change();
            });
    connect(spn_kerning_value_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, emit_change](double v) {
                if (can_edit()) { layer_->manual_kerning = (float)v; emit_change(); }
            });
    connect(spn_text_leading_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, emit_change](double v){
                if (can_edit()) { layer_->text_leading = (float)v; emit_change(); }
            });
    connect(spn_char_tracking_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, emit_change](double v){
                if (can_edit()) { layer_->char_tracking = (float)v; emit_change(); }
            });
    connect(spn_char_scale_x_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, emit_change](double v){
                if (can_edit()) { layer_->char_scale_x = (float)(v / 100.0); emit_change(); }
            });
    connect(spn_char_scale_y_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, emit_change](double v){
                if (can_edit()) { layer_->char_scale_y = (float)(v / 100.0); emit_change(); }
            });
    connect(spn_baseline_shift_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, emit_change](double v){
                if (can_edit()) { layer_->baseline_shift = (float)v; emit_change(); }
            });
    connect(cmb_language_, &QComboBox::currentTextChanged,
            this, [this, can_edit, emit_change](const QString &s){
                if (can_edit()) { layer_->text_language = s.toStdString(); emit_change(); }
            });
    auto set_exclusive_text_style = [this, can_edit, emit_change](int style, bool checked) {
        if (!can_edit() || !checked) return;
        layer_->text_style = style;
        emit_change();
        load_values();
    };
    connect(btn_all_caps_, &QToolButton::toggled, this, [set_exclusive_text_style](bool v){ set_exclusive_text_style(1, v); });
    connect(btn_small_caps_, &QToolButton::toggled, this, [set_exclusive_text_style](bool v){ set_exclusive_text_style(2, v); });
    connect(btn_superscript_, &QToolButton::toggled, this, [set_exclusive_text_style](bool v){ set_exclusive_text_style(3, v); });
    connect(btn_subscript_, &QToolButton::toggled, this, [set_exclusive_text_style](bool v){ set_exclusive_text_style(4, v); });
    connect(btn_underline_, &QToolButton::toggled, this, [this, can_edit, emit_change](bool v){ if (can_edit()) { layer_->text_underline = v; emit_change(); }});
    connect(btn_strikethrough_, &QToolButton::toggled, this, [this, can_edit, emit_change](bool v){ if (can_edit()) { layer_->text_strikethrough = v; emit_change(); }});
    connect(btn_ligatures_, &QToolButton::toggled, this, [this, can_edit, emit_change](bool v){ if (can_edit()) { layer_->text_ligatures = v; emit_change(); }});
    connect(btn_stylistic_alternates_, &QToolButton::toggled, this, [this, can_edit, emit_change](bool v){ if (can_edit()) { layer_->text_stylistic_alternates = v; emit_change(); }});
    connect(btn_fractions_, &QToolButton::toggled, this, [this, can_edit, emit_change](bool v){ if (can_edit()) { layer_->text_fractions = v; emit_change(); }});
    connect(btn_opentype_features_, &QToolButton::toggled, this, [this, can_edit, emit_change](bool v){ if (can_edit()) { layer_->text_opentype_features = v; emit_change(); }});
    connect(cmb_text_style_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this, can_edit, emit_change](int idx) {
                if (can_edit()) { layer_->text_style = cmb_text_style_->itemData(idx).toInt(); emit_change(); }
            });
    connect(cmb_text_overflow_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this, can_edit, emit_change](int idx) {
                if (can_edit()) { layer_->text_overflow_mode = cmb_text_overflow_->itemData(idx).toInt(); emit_change(); }
            });
    connect(spn_text_fit_min_scale_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, emit_change](double v) {
                if (can_edit()) { layer_->text_fit_min_scale = (float)v; emit_change(); }
            });
    connect(cmb_ticker_style_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this, can_edit, emit_change](int idx) {
                if (can_edit()) { layer_->ticker_style = cmb_ticker_style_->itemData(idx).toInt(); emit_change(); load_values(); }
            });
    connect(spn_ticker_speed_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, emit_change](double v) {
                if (can_edit()) { layer_->ticker_speed = v; emit_change(); }
            });
    connect(spn_ticker_line_hold_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, emit_change](double v) {
                if (can_edit()) { layer_->ticker_line_hold = v; emit_change(); }
            });
    connect(cmb_ticker_direction_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this, can_edit, emit_change](int idx) {
                if (can_edit()) { layer_->ticker_direction = cmb_ticker_direction_->itemData(idx).toInt(); emit_change(); }
            });
    connect(chk_expose_text_, &QCheckBox::toggled,
            this, [this, can_edit, emit_change](bool v){
                if (can_edit()) { layer_->expose_text = v; emit_change(); }
            });
    connect(cmb_text_align_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this, can_edit, emit_change](int idx) {
                if (can_edit()) { layer_->align_h = cmb_text_align_->itemData(idx).toInt(); emit_change(); }
            });
    connect(cmb_text_valign_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this, can_edit, emit_change](int idx) {
                if (can_edit()) { layer_->align_v = cmb_text_valign_->itemData(idx).toInt(); emit_change(); }
            });
    connect(btn_text_color_, &QPushButton::clicked,
            this, [this, can_edit, local_time, emit_change]() {
                if (!can_edit()) return;
                QColor initial = color_from_argb(eval_text_color(*layer_, local_time()));
                QColor picked = QColorDialog::getColor(initial, this, obsgs_tr("OBSTitles.TextColor"),
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
        QColor picked = QColorDialog::getColor(color_from_argb(eval_shadow_color(*layer_, local_time())), this, obsgs_tr("OBSTitles.ShadowColor"), QColorDialog::ShowAlphaChannel);
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
                if (layer_->type == LayerType::Image && layer_->lock_aspect_ratio && old_w > 0.0) {
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
                QColor picked = QColorDialog::getColor(initial, this, obsgs_tr("OBSTitles.FillColor"),
                                                        QColorDialog::ShowAlphaChannel);
                if (!picked.isValid()) return;
                layer_->fill_color = argb_from_color(picked);
                set_color_channels_at(*layer_, false, local_time(), layer_->fill_color);
                style_color_button(btn_fill_color_, layer_->fill_color);
                emit_change();
            });
    connect(cmb_fill_type_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this, can_edit, emit_change](int idx) {
                if (!can_edit()) return;
                layer_->fill_type = cmb_fill_type_->itemData(idx).toInt();
                load_values();
                emit_change();
            });
    connect(cmb_gradient_type_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this, can_edit, emit_change](int idx) {
                if (can_edit()) { layer_->gradient_type = cmb_gradient_type_->itemData(idx).toInt(); emit_change(); }
            });
    auto connect_gradient_color = [this, can_edit, emit_change](QPushButton *button, uint32_t Layer::*member,
                                                                 const char *title_key) {
        connect(button, &QPushButton::clicked, this, [this, can_edit, emit_change, button, member, title_key]() {
            if (!can_edit()) return;
            QColor picked = QColorDialog::getColor(color_from_argb((*layer_).*member), this, obsgs_tr(title_key),
                                                    QColorDialog::ShowAlphaChannel);
            if (!picked.isValid()) return;
            (*layer_).*member = argb_from_color(picked);
            style_color_button(button, (*layer_).*member);
            emit_change();
        });
    };
    connect_gradient_color(btn_gradient_start_color_, &Layer::gradient_start_color, "OBSTitles.StartColorLabel");
    connect_gradient_color(btn_gradient_end_color_, &Layer::gradient_end_color, "OBSTitles.EndColorLabel");
    connect(spn_gradient_start_pos_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, emit_change](double v) { if (can_edit()) { layer_->gradient_start_pos = (float)v; emit_change(); } });
    connect(spn_gradient_end_pos_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, emit_change](double v) { if (can_edit()) { layer_->gradient_end_pos = (float)v; emit_change(); } });
    connect(spn_gradient_start_opacity_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, emit_change](double v) { if (can_edit()) { layer_->gradient_start_opacity = (float)v; emit_change(); } });
    connect(spn_gradient_end_opacity_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, emit_change](double v) { if (can_edit()) { layer_->gradient_end_opacity = (float)v; emit_change(); } });
    connect(spn_gradient_opacity_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, emit_change](double v) { if (can_edit()) { layer_->gradient_opacity = (float)v; emit_change(); } });
    connect(spn_gradient_angle_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, emit_change](double v) { if (can_edit()) { layer_->gradient_angle = (float)v; emit_change(); } });
    connect(spn_gradient_center_x_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, emit_change](double v) { if (can_edit()) { layer_->gradient_center_x = (float)v; emit_change(); } });
    connect(spn_gradient_center_y_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, emit_change](double v) { if (can_edit()) { layer_->gradient_center_y = (float)v; emit_change(); } });
    connect(spn_gradient_scale_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, emit_change](double v) { if (can_edit()) { layer_->gradient_scale = (float)v; emit_change(); } });
    connect(spn_gradient_focal_x_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, emit_change](double v) { if (can_edit()) { layer_->gradient_focal_x = (float)v; emit_change(); } });
    connect(spn_gradient_focal_y_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, emit_change](double v) { if (can_edit()) { layer_->gradient_focal_y = (float)v; emit_change(); } });
    connect(chk_background_enabled_, &QCheckBox::toggled,
            this, [this, can_edit, local_time, emit_change](bool v) {
                if (can_edit()) { layer_->background_enabled = v; set_animated_value(layer_->background_enabled_prop, local_time(), v ? 1.0 : 0.0); emit_change(); }
            });
    connect(btn_background_color_, &QPushButton::clicked,
            this, [this, can_edit, local_time, emit_change]() {
                if (!can_edit()) return;
                QColor picked = QColorDialog::getColor(color_from_argb(eval_background_color(*layer_, local_time())), this,
                                                        obsgs_tr("OBSTitles.BackgroundColor"),
                                                        QColorDialog::ShowAlphaChannel);
                if (!picked.isValid()) return;
                layer_->background_color = argb_from_color(picked);
                set_background_color_channels_at(*layer_, local_time(), layer_->background_color);
                style_color_button(btn_background_color_, layer_->background_color);
                emit_change();
            });
    connect(spn_background_opacity_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, local_time, emit_change](double v) {
                if (can_edit()) { layer_->background_opacity = (float)v; set_animated_value(layer_->background_opacity_prop, local_time(), v); emit_change(); }
            });
    connect(spn_background_padding_x_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, local_time, emit_change](double v) {
                if (can_edit()) { layer_->background_padding_x = (float)v; set_animated_value(layer_->background_padding_x_prop, local_time(), v); emit_change(); }
            });
    connect(spn_background_padding_y_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, local_time, emit_change](double v) {
                if (can_edit()) { layer_->background_padding_y = (float)v; set_animated_value(layer_->background_padding_y_prop, local_time(), v); emit_change(); }
            });
    connect(spn_background_corner_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, local_time, emit_change](double v) {
                if (can_edit()) { layer_->background_corner_radius = (float)v; set_animated_value(layer_->background_corner_radius_prop, local_time(), v); emit_change(); }
            });

    connect(chk_outline_enabled_, &QCheckBox::toggled,
            this, [this, can_edit, emit_change](bool v) {
                if (can_edit()) { layer_->outline_enabled = v; emit_change(); }
            });
    connect(spn_outline_width_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, emit_change](double v) {
                if (can_edit()) { layer_->stroke_width = (float)v; emit_change(); }
            });
    connect(spn_outline_opacity_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, emit_change](double v) {
                if (can_edit()) { layer_->outline_opacity = (float)v; emit_change(); }
            });
    connect(cmb_outline_join_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this, can_edit, emit_change](int idx) {
                if (can_edit()) { layer_->outline_join_style = cmb_outline_join_->itemData(idx).toInt(); emit_change(); }
            });
    connect(cmb_outline_position_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this, can_edit, emit_change](int idx) {
                if (can_edit()) { layer_->outline_on_front = cmb_outline_position_->itemData(idx).toInt() != 0; emit_change(); }
            });
    connect(chk_outline_antialias_, &QCheckBox::toggled,
            this, [this, can_edit, emit_change](bool v) {
                if (can_edit()) { layer_->outline_antialias = v; emit_change(); }
            });
    connect(btn_outline_color_, &QPushButton::clicked,
            this, [this, can_edit, emit_change]() {
                if (!can_edit()) return;
                QColor initial = color_from_argb(layer_->stroke_color);
                QColor picked = QColorDialog::getColor(initial, this, obsgs_tr("OBSTitles.OutlineColor"),
                                                        QColorDialog::ShowAlphaChannel);
                if (!picked.isValid()) return;
                layer_->stroke_color = argb_from_color(picked);
                style_color_button(btn_outline_color_, layer_->stroke_color);
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
                    this, obsgs_tr("OBSTitles.ChooseImage"),
                    QString::fromStdString(layer_->image_path),
                    obsgs_tr("OBSTitles.ImageFileFilter"));
                if (path.isEmpty()) return;
                layer_->image_path = path.toStdString();
                QSize image_size = editor_image_intrinsic_size(path);
                if (image_size.isValid() && !image_size.isEmpty()) {
                    double t = local_time();
                    layer_->rect_width = (float)image_size.width();
                    layer_->rect_height = (float)image_size.height();
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
    connect(btn_kf_scale_x_, &QPushButton::clicked, this, [this, can_edit, local_time, emit_change]() {
        if (!can_edit()) return;
        const double t = local_time();
        if (layer_->scale_lock) {
            const bool remove = keyframe_at_time(layer_->scale_x, t) || keyframe_at_time(layer_->scale_y, t);
            if (remove) {
                remove_keyframe_at(layer_->scale_x, t);
                remove_keyframe_at(layer_->scale_y, t);
            } else {
                add_or_replace_keyframe(layer_->scale_x, t, spn_scale_x_->value() / 100.0);
                add_or_replace_keyframe(layer_->scale_y, t, spn_scale_y_->value() / 100.0);
            }
        } else {
            toggle_keyframe(layer_->scale_x, t, spn_scale_x_->value() / 100.0);
        }
        load_values();
        emit_change();
    });
    connect(btn_kf_scale_y_, &QPushButton::clicked, this, [this, can_edit, local_time, emit_change]() {
        if (!can_edit()) return;
        const double t = local_time();
        if (layer_->scale_lock) {
            const bool remove = keyframe_at_time(layer_->scale_x, t) || keyframe_at_time(layer_->scale_y, t);
            if (remove) {
                remove_keyframe_at(layer_->scale_x, t);
                remove_keyframe_at(layer_->scale_y, t);
            } else {
                add_or_replace_keyframe(layer_->scale_x, t, spn_scale_x_->value() / 100.0);
                add_or_replace_keyframe(layer_->scale_y, t, spn_scale_y_->value() / 100.0);
            }
        } else {
            toggle_keyframe(layer_->scale_y, t, spn_scale_y_->value() / 100.0);
        }
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

    connect(btn_kf_background_enabled_, &QPushButton::clicked, this, [this, can_edit, local_time, emit_change]() {
        if (!can_edit()) return;
        toggle_keyframe(layer_->background_enabled_prop, local_time(), chk_background_enabled_->isChecked() ? 1.0 : 0.0);
        load_values();
        emit_change();
    });
    connect(btn_kf_background_opacity_, &QPushButton::clicked, this, [this, can_edit, local_time, emit_change]() {
        if (!can_edit()) return;
        toggle_keyframe(layer_->background_opacity_prop, local_time(), spn_background_opacity_->value());
        load_values();
        emit_change();
    });
    connect(btn_kf_background_padding_x_, &QPushButton::clicked, this, [this, can_edit, local_time, emit_change]() {
        if (!can_edit()) return;
        toggle_keyframe(layer_->background_padding_x_prop, local_time(), spn_background_padding_x_->value());
        load_values();
        emit_change();
    });
    connect(btn_kf_background_padding_y_, &QPushButton::clicked, this, [this, can_edit, local_time, emit_change]() {
        if (!can_edit()) return;
        toggle_keyframe(layer_->background_padding_y_prop, local_time(), spn_background_padding_y_->value());
        load_values();
        emit_change();
    });
    connect(btn_kf_background_corner_, &QPushButton::clicked, this, [this, can_edit, local_time, emit_change]() {
        if (!can_edit()) return;
        toggle_keyframe(layer_->background_corner_radius_prop, local_time(), spn_background_corner_->value());
        load_values();
        emit_change();
    });
    connect(btn_kf_background_color_, &QPushButton::clicked, this, [this, can_edit, local_time, emit_change]() {
        if (!can_edit()) return;
        double t = local_time();
        uint32_t color = eval_background_color(*layer_, t);
        if (any_keyframe_at_time({&layer_->background_color_a, &layer_->background_color_r,
                                  &layer_->background_color_g, &layer_->background_color_b}, t)) {
            remove_keyframe_at(layer_->background_color_a, t);
            remove_keyframe_at(layer_->background_color_r, t);
            remove_keyframe_at(layer_->background_color_g, t);
            remove_keyframe_at(layer_->background_color_b, t);
        } else {
            add_or_replace_keyframe(layer_->background_color_a, t, (color >> 24) & 0xFF);
            add_or_replace_keyframe(layer_->background_color_r, t, (color >> 16) & 0xFF);
            add_or_replace_keyframe(layer_->background_color_g, t, (color >> 8) & 0xFF);
            add_or_replace_keyframe(layer_->background_color_b, t, color & 0xFF);
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
        if (type_options_box_) type_options_box_->setVisible(false);
        if (paragraph_box_) paragraph_box_->setVisible(false);
        if (dynamic_text_box_) dynamic_text_box_->setVisible(false);
        if (bullets_box_) bullets_box_->setVisible(false);
        rect_box_->setVisible(false);
        if (gradient_box_) gradient_box_->setVisible(false);
        image_box_->setVisible(false);
        if (outline_box_) outline_box_->setVisible(false);
        if (shadow_box_) shadow_box_->setVisible(false);
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
        if (btn_text_color_) btn_text_color_->setEnabled(true);
        if (btn_kf_text_color_) btn_kf_text_color_->setEnabled(true);
        style_color_button(btn_fill_color_, 0xFF222222);
        if (cmb_fill_type_) cmb_fill_type_->setCurrentIndex(0);
        if (cmb_gradient_type_) cmb_gradient_type_->setCurrentIndex(0);
        if (btn_gradient_start_color_) style_color_button(btn_gradient_start_color_, 0xFF4B6EA8);
        if (btn_gradient_end_color_) style_color_button(btn_gradient_end_color_, 0xFF1B1B1B);
        if (spn_gradient_start_pos_) spn_gradient_start_pos_->setValue(0.0);
        if (spn_gradient_end_pos_) spn_gradient_end_pos_->setValue(1.0);
        if (spn_gradient_start_opacity_) spn_gradient_start_opacity_->setValue(1.0);
        if (spn_gradient_end_opacity_) spn_gradient_end_opacity_->setValue(1.0);
        if (spn_gradient_opacity_) spn_gradient_opacity_->setValue(1.0);
        if (spn_gradient_angle_) spn_gradient_angle_->setValue(0.0);
        if (spn_gradient_center_x_) spn_gradient_center_x_->setValue(0.5);
        if (spn_gradient_center_y_) spn_gradient_center_y_->setValue(0.5);
        if (spn_gradient_scale_) spn_gradient_scale_->setValue(1.0);
        if (spn_gradient_focal_x_) spn_gradient_focal_x_->setValue(0.5);
        if (spn_gradient_focal_y_) spn_gradient_focal_y_->setValue(0.5);
        if (chk_background_enabled_) chk_background_enabled_->setChecked(false);
        if (btn_background_color_) style_color_button(btn_background_color_, 0xFF000000);
        if (spn_background_opacity_) spn_background_opacity_->setValue(0.35);
        if (spn_background_padding_x_) spn_background_padding_x_->setValue(0.0);
        if (spn_background_padding_y_) spn_background_padding_y_->setValue(0.0);
        if (spn_background_corner_) spn_background_corner_->setValue(0.0);
        if (chk_outline_enabled_) chk_outline_enabled_->setChecked(false);
        if (btn_outline_color_) style_color_button(btn_outline_color_, 0xFF000000);
        if (spn_outline_width_) spn_outline_width_->setValue(0.0);
        if (spn_outline_opacity_) spn_outline_opacity_->setValue(1.0);
        if (cmb_outline_join_) cmb_outline_join_->setCurrentIndex(1);
        if (cmb_outline_position_) cmb_outline_position_->setCurrentIndex(1);
        if (chk_outline_antialias_) chk_outline_antialias_->setChecked(true);
        spn_layer_w_->setValue(0.0);
        spn_layer_h_->setValue(0.0);
        spn_rect_corner_->setValue(0.0);
        spn_size_->setValue(72);
        if (cmb_font_style_) populate_font_style_combo(cmb_font_style_, cmb_font_->currentText(), QStringLiteral("Regular"));
        chk_bold_->setChecked(false);
        chk_italic_->setChecked(false);
        if (chk_font_kerning_) chk_font_kerning_->setChecked(true);
        if (spn_text_leading_) spn_text_leading_->setValue(0.0);
        if (spn_char_tracking_) spn_char_tracking_->setValue(0.0);
        if (cmb_kerning_mode_) cmb_kerning_mode_->setCurrentIndex(0);
        if (spn_kerning_value_) spn_kerning_value_->setValue(0.0);
        if (spn_scale_x_) spn_scale_x_->setValue(100.0);
        if (spn_scale_y_) spn_scale_y_->setValue(100.0);
        if (chk_scale_lock_) chk_scale_lock_->setChecked(true);
        if (spn_char_scale_x_) spn_char_scale_x_->setValue(100.0);
        if (spn_char_scale_y_) spn_char_scale_y_->setValue(100.0);
        if (spn_baseline_shift_) spn_baseline_shift_->setValue(0.0);
        if (cmb_language_) cmb_language_->setCurrentIndex(0);
        for (auto *b : {btn_all_caps_, btn_small_caps_, btn_superscript_, btn_subscript_, btn_underline_,
                        btn_strikethrough_, btn_ligatures_, btn_stylistic_alternates_, btn_fractions_, btn_opentype_features_})
            if (b) b->setChecked(false);
        if (cmb_text_style_) cmb_text_style_->setCurrentIndex(0);
        if (cmb_text_overflow_) cmb_text_overflow_->setCurrentIndex(0);
        if (spn_text_fit_min_scale_) spn_text_fit_min_scale_->setValue(0.5);
        if (lbl_text_fit_scale_) lbl_text_fit_scale_->setText(obsgs_tr("OBSTitles.Scale100"));
        if (chk_text_box_width_to_text_) chk_text_box_width_to_text_->setChecked(false);
        if (chk_text_box_height_to_text_) chk_text_box_height_to_text_->setChecked(false);
        if (spn_max_text_box_width_) { spn_max_text_box_width_->setValue(1920.0); spn_max_text_box_width_->setEnabled(false); }
        if (spn_max_text_box_height_) { spn_max_text_box_height_->setValue(1080.0); spn_max_text_box_height_->setEnabled(false); }
        if (cmb_text_align_) cmb_text_align_->setCurrentIndex(1);
        if (cmb_text_valign_) cmb_text_valign_->setCurrentIndex(1);
        if (cmb_anchor_) cmb_anchor_->setCurrentIndex(4);
        if (chk_shadow_enabled_) chk_shadow_enabled_->setChecked(false);
        if (cmb_shadow_preset_) cmb_shadow_preset_->setCurrentIndex(0);
        if (btn_shadow_color_) style_color_button(btn_shadow_color_, 0x99000000);
        if (spn_shadow_opacity_) spn_shadow_opacity_->setValue(0.6);
        if (spn_shadow_distance_) spn_shadow_distance_->setValue(8.0);
        if (spn_shadow_angle_) spn_shadow_angle_->setValue(135.0);
        if (spn_shadow_blur_) spn_shadow_blur_->setValue(4.0);
        if (spn_shadow_spread_) spn_shadow_spread_->setValue(0.0);
        for (auto *b : {btn_kf_pos_x_, btn_kf_pos_y_, btn_kf_scale_x_, btn_kf_scale_y_,
                        btn_kf_rotation_, btn_kf_opacity_, btn_kf_origin_x_, btn_kf_origin_y_,
                        btn_kf_width_, btn_kf_height_,
                        btn_kf_text_color_, btn_kf_fill_color_, btn_kf_background_enabled_,
                        btn_kf_background_color_, btn_kf_background_opacity_, btn_kf_background_padding_x_,
                        btn_kf_background_padding_y_, btn_kf_background_corner_, btn_kf_shadow_enabled_,
                        btn_kf_shadow_opacity_, btn_kf_shadow_distance_, btn_kf_shadow_angle_,
                        btn_kf_shadow_blur_, btn_kf_shadow_spread_, btn_kf_shadow_color_}) {
            if (!b) continue;
            b->setIcon(keyframe_diamond_icon(false));
            b->setProperty("active", false);
            b->setProperty("outlined", false);
            b->style()->unpolish(b);
            b->style()->polish(b);
        }
        loading_values_ = false;
        return;
    }

    const bool is_text = layer_->type == LayerType::Text;
    const bool is_clock = layer_->type == LayerType::Clock;
    const bool is_ticker = layer_->type == LayerType::Ticker;
    const bool is_text_like = is_text || is_clock || is_ticker;
    const bool is_rect = layer_->type == LayerType::SolidRect || layer_->type == LayerType::Shape;
    const bool is_image = layer_->type == LayerType::Image;
    const bool supports_outline = is_text_like || is_rect;
    text_box_->setVisible(is_text_like);
    if (type_options_box_) type_options_box_->setVisible(is_text_like);
    if (paragraph_box_) paragraph_box_->setVisible(is_text_like);
    if (dynamic_text_box_) dynamic_text_box_->setVisible(is_text_like);
    if (bullets_box_) bullets_box_->setVisible(is_text_like);
    text_box_->setTitle("Character");
    txt_content_->setPlaceholderText(is_clock ? "H:i:s" : obsgs_tr("OBSTitles.EnterTextPlaceholder"));
    if (spn_text_fit_min_scale_) spn_text_fit_min_scale_->setVisible(is_text_like && layer_->text_overflow_mode == 2 && !is_ticker);
    if (lbl_text_fit_scale_) lbl_text_fit_scale_->setVisible(is_text_like && layer_->text_overflow_mode == 2 && !is_ticker);
    if (auto *dynamic_form = qobject_cast<QFormLayout *>(dynamic_text_box_ ? dynamic_text_box_->layout() : nullptr)) {
        const bool show_ticker_fit = is_text_like && layer_->text_overflow_mode == 2 && !is_ticker;
        if (auto *label = dynamic_form->labelForField(spn_text_fit_min_scale_))
            label->setVisible(show_ticker_fit);
        if (cmb_ticker_style_) {
            cmb_ticker_style_->setVisible(is_ticker);
            if (auto *label = dynamic_form->labelForField(cmb_ticker_style_)) label->setVisible(is_ticker);
        }
        if (spn_ticker_speed_) {
            spn_ticker_speed_->setVisible(is_ticker && layer_->ticker_style != 1);
            if (auto *label = dynamic_form->labelForField(spn_ticker_speed_)) label->setVisible(is_ticker && layer_->ticker_style != 1);
        }
        if (spn_ticker_line_hold_) {
            spn_ticker_line_hold_->setVisible(is_ticker && layer_->ticker_style == 1);
            if (auto *label = dynamic_form->labelForField(spn_ticker_line_hold_)) label->setVisible(is_ticker && layer_->ticker_style == 1);
        }
        if (cmb_ticker_direction_) {
            cmb_ticker_direction_->setVisible(is_ticker);
            if (auto *label = dynamic_form->labelForField(cmb_ticker_direction_)) label->setVisible(is_ticker);
        }
        if (chk_expose_text_) {
            chk_expose_text_->setVisible(is_text || is_ticker);
            if (auto *label = dynamic_form->labelForField(chk_expose_text_))
                label->setVisible(is_text || is_ticker);
        }
    }
    rect_box_->setVisible(is_text_like || is_rect || is_image);
    rect_box_->setTitle(is_text_like ? (is_clock ? obsgs_tr("OBSTitles.ClockBox") : (is_ticker ? obsgs_tr("OBSTitles.TickerBox") : obsgs_tr("OBSTitles.TextBox"))) : (is_image ? obsgs_tr("OBSTitles.ImageSize") : obsgs_tr("OBSTitles.ShapeGeometryFill")));
    spn_rect_corner_->setVisible(is_rect);
    const bool supports_fill_type = is_rect || is_text_like || is_image;
    if (cmb_fill_type_) cmb_fill_type_->setVisible(supports_fill_type);
    const bool solid_fill_active = is_rect && layer_->fill_type == 0;
    btn_fill_color_->setVisible(solid_fill_active);
    if (gradient_box_) gradient_box_->setVisible(supports_fill_type && layer_->fill_type == 1);
    const bool supports_text_box_auto_size = is_text || is_clock;
    if (chk_text_box_width_to_text_) chk_text_box_width_to_text_->setVisible(supports_text_box_auto_size);
    if (chk_text_box_height_to_text_) chk_text_box_height_to_text_->setVisible(supports_text_box_auto_size);
    if (spn_max_text_box_width_) spn_max_text_box_width_->setVisible(supports_text_box_auto_size);
    if (spn_max_text_box_height_) spn_max_text_box_height_->setVisible(supports_text_box_auto_size);
    btn_kf_text_color_->setVisible(is_text_like);
    const bool gradient_text_active = is_text_like && layer_->fill_type == 1;
    if (btn_text_color_) btn_text_color_->setEnabled(!gradient_text_active);
    if (btn_kf_text_color_) btn_kf_text_color_->setEnabled(!gradient_text_active);
    btn_kf_fill_color_->setVisible(solid_fill_active);
    if (row_fill_type_) row_fill_type_->setVisible(supports_fill_type);
    if (row_fill_color_) row_fill_color_->setVisible(solid_fill_active);
    const bool supports_background = is_text_like || is_image;
    if (chk_background_enabled_) chk_background_enabled_->setVisible(supports_background);
    if (btn_background_color_) btn_background_color_->setVisible(supports_background);
    if (spn_background_opacity_) spn_background_opacity_->setVisible(supports_background);
    if (spn_background_padding_x_) spn_background_padding_x_->setVisible(supports_background);
    if (spn_background_padding_y_) spn_background_padding_y_->setVisible(supports_background);
    if (spn_background_corner_) spn_background_corner_->setVisible(supports_background);
    for (QPushButton *button : std::initializer_list<QPushButton *>{btn_kf_background_enabled_, btn_kf_background_color_,
                                                                    btn_kf_background_opacity_, btn_kf_background_padding_x_,
                                                                    btn_kf_background_padding_y_, btn_kf_background_corner_})
        if (button) button->setVisible(supports_background);
    if (outline_box_) outline_box_->setVisible(supports_outline);
    if (auto *outline_form = qobject_cast<QFormLayout *>(outline_box_->layout())) {
        if (btn_outline_color_) btn_outline_color_->setVisible(supports_outline);
        if (auto *label = outline_form->labelForField(btn_outline_color_))
            label->setVisible(supports_outline);
        if (spn_outline_width_) spn_outline_width_->setVisible(supports_outline);
        if (auto *label = outline_form->labelForField(spn_outline_width_))
            label->setVisible(supports_outline);
    }
    if (auto *form = qobject_cast<QFormLayout *>(rect_box_->layout())) {
        if (auto *label = form->labelForField(spn_rect_corner_))
            label->setVisible(is_rect);
        if (auto *label = form->labelForField(row_fill_type_))
            label->setVisible(supports_fill_type);
        if (auto *label = form->labelForField(row_fill_color_))
            label->setVisible(solid_fill_active);
        for (QWidget *field : std::initializer_list<QWidget *>{chk_text_box_width_to_text_, spn_max_text_box_width_,
                                                               chk_text_box_height_to_text_, spn_max_text_box_height_})
            if (auto *label = form->labelForField(field)) label->setVisible(supports_text_box_auto_size);
        for (QWidget *field : std::initializer_list<QWidget *>{row_background_enabled_, row_background_color_,
                                                               row_background_opacity_, row_background_padding_x_,
                                                               row_background_padding_y_, row_background_corner_}) {
            if (field) field->setVisible(supports_background);
            if (auto *label = form->labelForField(field)) label->setVisible(supports_background);
        }
        if (auto *label = form->labelForField(spn_outline_width_))
            label->setVisible(supports_outline);
        if (auto *label = form->labelForField(row_outline_color_))
            label->setVisible(supports_outline);
    }
    image_box_->setVisible(is_image);
    if (shadow_box_) shadow_box_->setVisible(true);

    double lt = std::clamp(playhead_ - layer_->in_time, 0.0,
                           std::max(0.0, layer_->out_time - layer_->in_time));
    spn_px_->setValue(layer_->pos_x.is_animated()
                      ? layer_->pos_x.evaluate(lt)
                      : layer_->pos_x.static_value);
    spn_py_->setValue(layer_->pos_y.is_animated()
                      ? layer_->pos_y.evaluate(lt)
                      : layer_->pos_y.static_value);
    spn_scale_x_->setValue((layer_->scale_x.is_animated()
                            ? layer_->scale_x.evaluate(lt)
                            : layer_->scale_x.static_value) * 100.0);
    spn_scale_y_->setValue((layer_->scale_y.is_animated()
                            ? layer_->scale_y.evaluate(lt)
                            : layer_->scale_y.static_value) * 100.0);
    if (chk_scale_lock_) chk_scale_lock_->setChecked(layer_->scale_lock);
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
    if (cmb_fill_type_) {
        int fill_idx = cmb_fill_type_->findData(layer_->fill_type);
        cmb_fill_type_->setCurrentIndex(fill_idx >= 0 ? fill_idx : 0);
    }
    if (cmb_gradient_type_) {
        int gradient_idx = cmb_gradient_type_->findData(layer_->gradient_type);
        cmb_gradient_type_->setCurrentIndex(gradient_idx >= 0 ? gradient_idx : 0);
    }
    if (btn_gradient_start_color_) style_color_button(btn_gradient_start_color_, layer_->gradient_start_color);
    if (btn_gradient_end_color_) style_color_button(btn_gradient_end_color_, layer_->gradient_end_color);
    if (spn_gradient_start_pos_) spn_gradient_start_pos_->setValue(layer_->gradient_start_pos);
    if (spn_gradient_end_pos_) spn_gradient_end_pos_->setValue(layer_->gradient_end_pos);
    if (spn_gradient_start_opacity_) spn_gradient_start_opacity_->setValue(layer_->gradient_start_opacity);
    if (spn_gradient_end_opacity_) spn_gradient_end_opacity_->setValue(layer_->gradient_end_opacity);
    if (spn_gradient_opacity_) spn_gradient_opacity_->setValue(layer_->gradient_opacity);
    if (spn_gradient_angle_) spn_gradient_angle_->setValue(layer_->gradient_angle);
    if (spn_gradient_center_x_) spn_gradient_center_x_->setValue(layer_->gradient_center_x);
    if (spn_gradient_center_y_) spn_gradient_center_y_->setValue(layer_->gradient_center_y);
    if (spn_gradient_scale_) spn_gradient_scale_->setValue(layer_->gradient_scale);
    if (spn_gradient_focal_x_) spn_gradient_focal_x_->setValue(layer_->gradient_focal_x);
    if (spn_gradient_focal_y_) spn_gradient_focal_y_->setValue(layer_->gradient_focal_y);
    if (chk_text_box_width_to_text_) chk_text_box_width_to_text_->setChecked(layer_->text_box_width_to_text);
    if (chk_text_box_height_to_text_) chk_text_box_height_to_text_->setChecked(layer_->text_box_height_to_text);
    if (spn_max_text_box_width_) { spn_max_text_box_width_->setValue(layer_->max_text_box_width); spn_max_text_box_width_->setEnabled(layer_->text_box_width_to_text); }
    if (spn_max_text_box_height_) { spn_max_text_box_height_->setValue(layer_->max_text_box_height); spn_max_text_box_height_->setEnabled(layer_->text_box_height_to_text); }
    if (chk_background_enabled_) chk_background_enabled_->setChecked(eval_background_enabled(*layer_, lt));
    if (btn_background_color_) style_color_button(btn_background_color_, eval_background_color(*layer_, lt));
    if (spn_background_opacity_) spn_background_opacity_->setValue(eval_background_opacity(*layer_, lt));
    if (spn_background_padding_x_) spn_background_padding_x_->setValue(eval_background_padding_x(*layer_, lt));
    if (spn_background_padding_y_) spn_background_padding_y_->setValue(eval_background_padding_y(*layer_, lt));
    if (spn_background_corner_) spn_background_corner_->setValue(eval_background_corner_radius(*layer_, lt));
    if (chk_outline_enabled_) chk_outline_enabled_->setChecked(layer_->outline_enabled);
    if (spn_outline_width_) spn_outline_width_->setValue(layer_->stroke_width);
    if (btn_outline_color_) style_color_button(btn_outline_color_, eval_outline_color(*layer_, lt));
    if (spn_outline_opacity_) spn_outline_opacity_->setValue(eval_outline_opacity(*layer_, lt));
    if (cmb_outline_join_) {
        int join_idx = cmb_outline_join_->findData(layer_->outline_join_style);
        cmb_outline_join_->setCurrentIndex(join_idx >= 0 ? join_idx : 1);
    }
    if (cmb_outline_position_) {
        int position_idx = cmb_outline_position_->findData(layer_->outline_on_front ? 1 : 0);
        cmb_outline_position_->setCurrentIndex(position_idx >= 0 ? position_idx : 1);
    }
    if (chk_outline_antialias_) chk_outline_antialias_->setChecked(layer_->outline_antialias);

    auto set_kf_icon = [](QPushButton *button, bool active, bool has_keyframes) {
        if (!button) return;
        const bool outlined = has_keyframes && !active;
        button->setIcon(keyframe_diamond_icon(active, outlined));
        button->setProperty("active", active);
        button->setProperty("outlined", outlined);
        button->style()->unpolish(button);
        button->style()->polish(button);
    };
    auto set_prop_kf_icon = [&](QPushButton *button, const AnimatedProperty &prop) {
        set_kf_icon(button, keyframe_at_time(prop, lt), prop.is_animated());
    };
    auto set_group_kf_icon = [&](QPushButton *button, std::initializer_list<const AnimatedProperty *> props) {
        set_kf_icon(button, any_keyframe_at_time(props, lt), any_keyframes(props));
    };
    set_prop_kf_icon(btn_kf_pos_x_, layer_->pos_x);
    set_prop_kf_icon(btn_kf_pos_y_, layer_->pos_y);
    set_prop_kf_icon(btn_kf_scale_x_, layer_->scale_x);
    set_prop_kf_icon(btn_kf_scale_y_, layer_->scale_y);
    set_prop_kf_icon(btn_kf_rotation_, layer_->rotation);
    set_prop_kf_icon(btn_kf_opacity_, layer_->opacity);
    set_prop_kf_icon(btn_kf_origin_x_, layer_->origin_x_prop);
    set_prop_kf_icon(btn_kf_origin_y_, layer_->origin_y_prop);
    set_prop_kf_icon(btn_kf_width_, layer_->box_width);
    set_prop_kf_icon(btn_kf_height_, layer_->box_height);
    set_group_kf_icon(btn_kf_text_color_, {&layer_->text_color_a, &layer_->text_color_r,
                                           &layer_->text_color_g, &layer_->text_color_b});
    set_group_kf_icon(btn_kf_fill_color_, {&layer_->fill_color_a, &layer_->fill_color_r,
                                           &layer_->fill_color_g, &layer_->fill_color_b});
    set_prop_kf_icon(btn_kf_background_enabled_, layer_->background_enabled_prop);
    set_group_kf_icon(btn_kf_background_color_, {&layer_->background_color_a, &layer_->background_color_r,
                                                 &layer_->background_color_g, &layer_->background_color_b});
    set_prop_kf_icon(btn_kf_background_opacity_, layer_->background_opacity_prop);
    set_prop_kf_icon(btn_kf_background_padding_x_, layer_->background_padding_x_prop);
    set_prop_kf_icon(btn_kf_background_padding_y_, layer_->background_padding_y_prop);
    set_prop_kf_icon(btn_kf_background_corner_, layer_->background_corner_radius_prop);
    set_prop_kf_icon(btn_kf_shadow_enabled_, layer_->shadow_enabled_prop);
    set_prop_kf_icon(btn_kf_shadow_opacity_, layer_->shadow_opacity_prop);
    set_prop_kf_icon(btn_kf_shadow_distance_, layer_->shadow_distance_prop);
    set_prop_kf_icon(btn_kf_shadow_angle_, layer_->shadow_angle_prop);
    set_prop_kf_icon(btn_kf_shadow_blur_, layer_->shadow_blur_prop);
    set_prop_kf_icon(btn_kf_shadow_spread_, layer_->shadow_spread_prop);
    set_group_kf_icon(btn_kf_shadow_color_, {&layer_->shadow_color_a, &layer_->shadow_color_r,
                                             &layer_->shadow_color_g, &layer_->shadow_color_b});

    txt_content_->setPlainText(QString::fromStdString(is_clock ? layer_->clock_format : layer_->text_content));
    int ticker_style_idx = cmb_ticker_style_->findData(layer_->ticker_style);
    cmb_ticker_style_->setCurrentIndex(ticker_style_idx >= 0 ? ticker_style_idx : 0);
    spn_ticker_speed_->setValue(layer_->ticker_speed);
    spn_ticker_line_hold_->setValue(layer_->ticker_line_hold);
    cmb_ticker_direction_->clear();
    if (layer_->ticker_style == 0) {
        cmb_ticker_direction_->addItem(obsgs_tr("OBSTitles.LeftToRight"), 0);
        cmb_ticker_direction_->addItem(obsgs_tr("OBSTitles.RightToLeft"), 1);
    } else {
        cmb_ticker_direction_->addItem(obsgs_tr("OBSTitles.TopToBottom"), 0);
        cmb_ticker_direction_->addItem(obsgs_tr("OBSTitles.BottomToTop"), 1);
    }
    int ticker_direction_idx = cmb_ticker_direction_->findData(layer_->ticker_direction);
    cmb_ticker_direction_->setCurrentIndex(ticker_direction_idx >= 0 ? ticker_direction_idx : 0);
    int fi = cmb_font_->findText(QString::fromStdString(layer_->font_family));
    if (fi >= 0) cmb_font_->setCurrentIndex(fi);
    populate_font_style_combo(cmb_font_style_, QString::fromStdString(layer_->font_family), QString::fromStdString(layer_->font_style));
    spn_size_->setValue(layer_->font_size);
    chk_bold_->setChecked(layer_->font_bold);
    chk_italic_->setChecked(layer_->font_italic);
    if (chk_font_kerning_) chk_font_kerning_->setChecked(layer_->font_kerning);
    if (cmb_kerning_mode_) {
        int ki = cmb_kerning_mode_->findData(layer_->kerning_mode);
        cmb_kerning_mode_->setCurrentIndex(ki >= 0 ? ki : 0);
    }
    if (spn_kerning_value_) {
        spn_kerning_value_->setValue(layer_->manual_kerning);
        spn_kerning_value_->setEnabled(layer_->kerning_mode == 2);
    }
    if (spn_text_leading_) spn_text_leading_->setValue(layer_->text_leading);
    if (spn_char_tracking_) spn_char_tracking_->setValue(layer_->char_tracking);
    if (spn_char_scale_x_) spn_char_scale_x_->setValue(layer_->char_scale_x * 100.0);
    if (spn_char_scale_y_) spn_char_scale_y_->setValue(layer_->char_scale_y * 100.0);
    if (spn_baseline_shift_) spn_baseline_shift_->setValue(layer_->baseline_shift);
    if (cmb_language_) {
        int li = cmb_language_->findText(QString::fromStdString(layer_->text_language));
        cmb_language_->setCurrentIndex(li >= 0 ? li : 0);
    }
    if (btn_all_caps_) btn_all_caps_->setChecked(layer_->text_style == 1);
    if (btn_small_caps_) btn_small_caps_->setChecked(layer_->text_style == 2);
    if (btn_superscript_) btn_superscript_->setChecked(layer_->text_style == 3);
    if (btn_subscript_) btn_subscript_->setChecked(layer_->text_style == 4);
    if (btn_underline_) btn_underline_->setChecked(layer_->text_underline);
    if (btn_strikethrough_) btn_strikethrough_->setChecked(layer_->text_strikethrough);
    if (btn_ligatures_) btn_ligatures_->setChecked(layer_->text_ligatures);
    if (btn_stylistic_alternates_) btn_stylistic_alternates_->setChecked(layer_->text_stylistic_alternates);
    if (btn_fractions_) btn_fractions_->setChecked(layer_->text_fractions);
    if (btn_opentype_features_) btn_opentype_features_->setChecked(layer_->text_opentype_features);
    int style_idx = cmb_text_style_->findData(layer_->text_style);
    cmb_text_style_->setCurrentIndex(style_idx >= 0 ? style_idx : 0);
    int overflow_idx = cmb_text_overflow_->findData(layer_->text_overflow_mode);
    cmb_text_overflow_->setCurrentIndex(overflow_idx >= 0 ? overflow_idx : 0);
    spn_text_fit_min_scale_->setValue(layer_->text_fit_min_scale);
    bool is_fit = layer_->text_overflow_mode == 2 && !is_ticker;
    spn_text_fit_min_scale_->setVisible(is_fit);
    lbl_text_fit_scale_->setVisible(is_fit);
    if (auto *form = qobject_cast<QFormLayout *>(dynamic_text_box_ ? dynamic_text_box_->layout() : nullptr)) {
        if (auto *label = form->labelForField(spn_text_fit_min_scale_))
            label->setVisible(is_fit);
    }
    if (lbl_text_fit_scale_) {
        QFont preview_font = font_for_layer(*layer_);
        QRectF preview_rect(0, 0, eval_box_width(*layer_, lt), eval_box_height(*layer_, lt));
        double scale = horizontal_fit_scale(preview_font, preview_rect, display_text_for_style(*layer_), *layer_);
        lbl_text_fit_scale_->setText(obsgs_tr("OBSTitles.ScalePercentFormat").arg((int)std::round(scale * 100.0)));
    }
    chk_expose_text_->setChecked(layer_->expose_text);
    int ai = cmb_text_align_->findData(layer_->align_h);
    cmb_text_align_->setCurrentIndex(ai >= 0 ? ai : 1);
    int vai = cmb_text_valign_->findData(layer_->align_v);
    cmb_text_valign_->setCurrentIndex(vai >= 0 ? vai : 1);

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
        : obsgs_tr("OBSTitles.FontMissingWarningFormat").arg(QString::fromStdString(layer_->font_family)));

    loading_values_ = false;
}
