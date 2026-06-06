/*
 * title-dock.cpp
 */

#include "title-dock.h"
#include "title-editor.h"
#include "title-data.h"
#include "title-source.h"
#include "title-assets.h"
#include "title-localization.h"

#include <obs-module.h>
#include <obs-frontend-api.h>

#include <QAction>
#include <QBuffer>
#include <QColor>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QInputDialog>
#include <QIODevice>
#include <QItemSelectionModel>
#include <QMenu>
#include <QMimeData>
#include <QTextEdit>
#include <QMessageBox>
#include <QVBoxLayout>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QIcon>
#include <QStyle>
#include <QStyleOptionButton>
#include <QToolButton>
#include <QToolBar>
#include <QDoubleSpinBox>
#include <QSettings>
#include <QWidgetAction>
#include <QPushButton>
#include <QListView>
#include <QListWidget>
#include <QTreeWidget>
#include <QUrl>
#include <QFont>
#include <QFrame>
#include <QSizePolicy>
#include <QString>
#include <QStringList>
#include <QHeaderView>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QPen>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTabWidget>
#include <QTableWidgetItem>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QRegularExpression>
#include <memory>
#include <algorithm>
#include <functional>
#include <numeric>
#include <iterator>
#include <array>
#include <cmath>

namespace {

constexpr int TemplateCategoryNameRole = Qt::UserRole + 1;
constexpr const char *kDockSettingsGroup = "TitleDock";
constexpr const char *kDockSplitterStateKey = "sectionSplitterState";
constexpr const char *kTemplateIconViewKey = "templateIconView";
constexpr const char *kPlaylistLoopKey = "playlistLoop";
constexpr const char *kPlaylistReverseKey = "playlistReverse";
constexpr const char *kPlaylistHoldSecondsKey = "playlistHoldSeconds";
constexpr const char *kBackgroundPersistenceKey = "backgroundPersistence";
constexpr const char *kTextPersistenceKey = "textPersistence";

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

static QString current_scene_collection_titles_label()
{
    char *collection_name = obs_frontend_get_current_scene_collection();
    QString name = QString::fromUtf8(collection_name ? collection_name : "").trimmed();
    bfree(collection_name);

    if (name.isEmpty())
        name = obsgs_tr("OBSTitles.SceneCollectionFallback");

    return obsgs_tr("OBSTitles.SceneCollectionTitlesAndGraphicsFormat").arg(name);
}

static QString live_text_layer_header(const std::shared_ptr<Layer> &layer)
{
    if (!layer) return obsgs_tr("OBSTitles.Text");
    QString name = QString::fromStdString(layer->name).trimmed();
    if (!name.isEmpty()) return name;
    name = QString::fromStdString(layer->text_content).trimmed();
    return name.isEmpty() ? obsgs_tr("OBSTitles.Text") : name;
}

static void normalize_live_text_rows(const std::shared_ptr<Title> &title,
                                     const std::vector<std::shared_ptr<Layer>> &exposed)
{
    if (!title || exposed.empty()) return;

    std::vector<std::string> new_order;
    new_order.reserve(exposed.size());
    for (const auto &layer : exposed)
        new_order.push_back(layer ? layer->id : std::string());

    const std::vector<std::string> old_order = title->live_text_column_order;
    if (!old_order.empty() && old_order != new_order) {
        for (auto &row : title->live_text_rows) {
            std::vector<std::string> remapped;
            remapped.reserve(exposed.size());
            for (size_t new_col = 0; new_col < new_order.size(); ++new_col) {
                auto it = std::find(old_order.begin(), old_order.end(), new_order[new_col]);
                if (it != old_order.end()) {
                    const size_t old_col = (size_t)std::distance(old_order.begin(), it);
                    remapped.push_back(old_col < row.size() ? row[old_col] : exposed[new_col]->text_content);
                } else {
                    remapped.push_back(exposed[new_col]->text_content);
                }
            }
            row = std::move(remapped);
        }
    }
    title->live_text_column_order = std::move(new_order);

    if (title->live_text_rows.empty()) {
        std::vector<std::string> row;
        for (const auto &layer : exposed)
            row.push_back(layer->text_content);
        title->live_text_rows.push_back(std::move(row));
    }
    for (auto &row : title->live_text_rows) {
        size_t old_size = row.size();
        row.resize(exposed.size());
        for (size_t i = old_size; i < exposed.size(); ++i)
            row[i] = exposed[i]->text_content;
    }
}


static QIcon obs_icon(const char *file_name)
{
    return obsgs_icon(file_name);
}

static std::string obs_text_std(const char *key)
{
    return obsgs_tr(key).toStdString();
}

static int obs_toolbar_icon_extent(QWidget *widget)
{
    int size = widget ? widget->style()->pixelMetric(QStyle::PM_SmallIconSize, nullptr, widget) : 0;
    return size > 0 ? size : 16;
}

static int obs_layout_spacing(QWidget *widget)
{
    int spacing = widget ? widget->style()->pixelMetric(QStyle::PM_LayoutVerticalSpacing, nullptr, widget) : -1;
    return spacing >= 0 ? spacing : 4;
}

static QToolBar *make_obs_dock_toolbar(QWidget *parent)
{
    auto *toolbar = new QToolBar(parent);
    toolbar->setMovable(false);
    toolbar->setFloatable(false);
    toolbar->setOrientation(Qt::Horizontal);
    toolbar->setToolButtonStyle(Qt::ToolButtonIconOnly);
    toolbar->setIconSize(QSize(obs_toolbar_icon_extent(parent), obs_toolbar_icon_extent(parent)));
    toolbar->setContentsMargins(0, 0, 0, 0);
    toolbar->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    return toolbar;
}

static QToolButton *make_obs_dock_tool_button(QWidget *parent, const QString &text,
                                              const QIcon &icon, const QString &tooltip)
{
    auto *button = new QToolButton(parent);
    button->setText(text);
    button->setAccessibleName(text);
    button->setToolTip(tooltip);
    button->setIcon(icon);
    button->setIconSize(QSize(obs_toolbar_icon_extent(parent), obs_toolbar_icon_extent(parent)));
    button->setToolButtonStyle(Qt::ToolButtonIconOnly);
    button->setAutoRaise(true);
    button->setFocusPolicy(Qt::StrongFocus);
    button->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    return button;
}

static QWidget *toolbar_spacer(QWidget *parent)
{
    auto *spacer = new QWidget(parent);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    return spacer;
}

static QIcon globe_status_icon(bool enabled, QWidget *widget)
{
    const int extent = obs_toolbar_icon_extent(widget);
    QPixmap pixmap(extent, extent);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QColor color = enabled ? QColor(38, 184, 79) : QColor(145, 145, 145);
    QPen pen(color, std::max(1, extent / 10));
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    const QRectF globe_rect(pen.widthF() / 2.0, pen.widthF() / 2.0,
                            extent - pen.widthF(), extent - pen.widthF());
    painter.drawEllipse(globe_rect);
    painter.drawLine(QPointF(extent / 2.0, globe_rect.top()), QPointF(extent / 2.0, globe_rect.bottom()));
    painter.drawArc(globe_rect.adjusted(extent * 0.22, 0, -extent * 0.22, 0), 90 * 16, 180 * 16);
    painter.drawArc(globe_rect.adjusted(extent * 0.22, 0, -extent * 0.22, 0), -90 * 16, 180 * 16);
    painter.drawLine(QPointF(globe_rect.left(), extent / 2.0), QPointF(globe_rect.right(), extent / 2.0));
    return QIcon(pixmap);
}

static QJsonArray live_text_rows_to_json(const std::vector<std::vector<std::string>> &rows)
{
    QJsonArray json_rows;
    for (const auto &row : rows) {
        QJsonArray json_row;
        for (const auto &cell : row)
            json_row.append(QString::fromStdString(cell));
        json_rows.append(json_row);
    }
    return json_rows;
}

static bool live_text_rows_from_json(const QJsonDocument &doc,
                                     std::vector<std::vector<std::string>> &rows,
                                     QString *error)
{
    QJsonArray json_rows;
    if (doc.isArray()) {
        json_rows = doc.array();
    } else if (doc.isObject()) {
        const QJsonObject root = doc.object();
        const QJsonValue rows_value = root.value(QStringLiteral("rows"));
        const QJsonValue live_rows_value = root.value(QStringLiteral("live_text_rows"));
        if (rows_value.isArray())
            json_rows = rows_value.toArray();
        else if (live_rows_value.isArray())
            json_rows = live_rows_value.toArray();
        else {
            if (error) *error = QStringLiteral("The JSON file must contain a rows or live_text_rows array.");
            return false;
        }
    } else {
        if (error) *error = QStringLiteral("The selected file is not a valid live text cue JSON document.");
        return false;
    }

    rows.clear();
    rows.reserve(json_rows.size());
    for (const QJsonValue &row_value : json_rows) {
        if (!row_value.isArray()) {
            if (error) *error = QStringLiteral("Each imported live text cue row must be a JSON array.");
            return false;
        }
        std::vector<std::string> row;
        const QJsonArray json_row = row_value.toArray();
        row.reserve(json_row.size());
        for (const QJsonValue &cell_value : json_row)
            row.push_back(cell_value.toVariant().toString().toStdString());
        rows.push_back(std::move(row));
    }
    return true;
}

static void set_bold_label(QLabel *label)
{
    if (!label) return;
    QFont font = label->font();
    font.setBold(true);
    label->setFont(font);
}


class LiveTextCueTable : public QTableWidget {
public:
    explicit LiveTextCueTable(QWidget *parent = nullptr)
        : QTableWidget(parent)
    {
        setMouseTracking(false);
        viewport()->setMouseTracking(false);
    }

protected:
    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (event && event->buttons() == Qt::NoButton)
            return;
        QTableWidget::mouseMoveEvent(event);
    }
};

class LiveTextCueHeader : public QHeaderView {
public:
    explicit LiveTextCueHeader(QWidget *parent = nullptr)
        : QHeaderView(Qt::Horizontal, parent)
    {
        setSectionsMovable(true);
        setSectionsClickable(true);
        setSectionResizeMode(QHeaderView::Interactive);
    }

    void set_select_all_checked(bool checked)
    {
        if (select_all_checked_ == checked) return;
        select_all_checked_ = checked;
        viewport()->update();
    }

    void set_select_all_visible(bool visible)
    {
        if (select_all_visible_ == visible) return;
        select_all_visible_ = visible;
        viewport()->update();
    }

    std::function<void(bool)> select_all_toggled;

protected:
    void paintSection(QPainter *painter, const QRect &rect, int logicalIndex) const override
    {
        QHeaderView::paintSection(painter, rect, logicalIndex);
        if (logicalIndex != 0 || !select_all_visible_) return;

        QStyleOptionButton option;
        option.state = QStyle::State_Enabled | (select_all_checked_ ? QStyle::State_On : QStyle::State_Off);
        option.rect = checkbox_rect(rect);
        style()->drawControl(QStyle::CE_CheckBox, &option, painter, this);
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (select_all_visible_ && event && event->button() == Qt::LeftButton && logicalIndexAt(event->pos()) == 0) {
            const QRect section_rect(sectionViewportPosition(0), 0, sectionSize(0), height());
            if (checkbox_rect(section_rect).contains(event->pos())) {
                select_all_checked_ = !select_all_checked_;
                viewport()->update();
                if (select_all_toggled)
                    select_all_toggled(select_all_checked_);
                return;
            }
        }

        QHeaderView::mousePressEvent(event);
    }

private:
    QRect checkbox_rect(const QRect &section_rect) const
    {
        const int indicator_width = style()->pixelMetric(QStyle::PM_IndicatorWidth, nullptr, this);
        const int indicator_height = style()->pixelMetric(QStyle::PM_IndicatorHeight, nullptr, this);
        return QRect(section_rect.x() + (section_rect.width() - indicator_width) / 2,
                     section_rect.y() + (section_rect.height() - indicator_height) / 2,
                     indicator_width,
                     indicator_height);
    }

    bool select_all_checked_ = false;
    bool select_all_visible_ = true;
};

static LiveTextCueHeader *live_text_cue_header(QTableWidget *table)
{
    return table ? dynamic_cast<LiveTextCueHeader *>(table->horizontalHeader()) : nullptr;
}

class TemplateListWidget : public QListWidget {
public:
    explicit TemplateListWidget(QWidget *parent = nullptr)
        : QListWidget(parent)
    {
        setDragEnabled(true);
        setAcceptDrops(false);
        setDragDropMode(QAbstractItemView::DragOnly);
        setDefaultDropAction(Qt::MoveAction);
    }

protected:
    QMimeData *mimeData(const QList<QListWidgetItem *> &items) const override
    {
        auto *mime = new QMimeData();
        QList<QUrl> urls;
        for (const auto *item : items) {
            if (!item) continue;
            const QString path = item->data(Qt::UserRole).toString();
            if (!path.isEmpty())
                urls << QUrl::fromLocalFile(path);
        }
        mime->setUrls(urls);
        return mime;
    }

    void startDrag(Qt::DropActions supported_actions) override
    {
        (void)supported_actions;
        const auto items = selectedItems();
        if (items.empty())
            return;

        std::unique_ptr<QMimeData> mime(mimeData(items));
        if (!mime || !mime->hasUrls())
            return;

        auto *drag = new QDrag(this);
        drag->setMimeData(mime.release());
        if (auto *item = currentItem()) {
            QPixmap pixmap = item->icon().pixmap(iconSize());
            if (!pixmap.isNull())
                drag->setPixmap(pixmap);
        }
        drag->exec(Qt::MoveAction);
    }
};

class TemplateCategoryTree : public QTreeWidget {
public:
    explicit TemplateCategoryTree(QWidget *parent = nullptr)
        : QTreeWidget(parent)
    {
        setAcceptDrops(true);
        viewport()->setAcceptDrops(true);
        setDropIndicatorShown(true);
        setDragDropMode(QAbstractItemView::DropOnly);
        setDefaultDropAction(Qt::MoveAction);
        setSelectionMode(QAbstractItemView::SingleSelection);
    }

    std::function<void()> templates_moved;
    std::function<void(QTreeWidgetItem *)> category_delete_requested;

protected:
    void dragEnterEvent(QDragEnterEvent *event) override
    {
        if (accept_template_drag(event))
            return;
        QTreeWidget::dragEnterEvent(event);
    }

    void dragMoveEvent(QDragMoveEvent *event) override
    {
        if (accept_template_drag(event))
            return;
        QTreeWidget::dragMoveEvent(event);
    }

    void dropEvent(QDropEvent *event) override
    {
        if (!event || !event->mimeData() || !event->mimeData()->hasUrls()) {
            QTreeWidget::dropEvent(event);
            return;
        }

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        auto *target = itemAt(event->position().toPoint());
#else
        auto *target = itemAt(event->pos());
#endif
        if (!target)
            target = currentItem();
        if (!target) return;

        const QString target_dir = target->data(0, Qt::UserRole).toString();
        if (target_dir.isEmpty()) return;
        QDir().mkpath(target_dir);

        bool moved_any = false;
        for (const QUrl &url : event->mimeData()->urls()) {
            const QString source_path = url.toLocalFile();
            if (source_path.isEmpty()) continue;
            QFileInfo source_info(source_path);
            if (!source_info.exists() || !source_info.isFile()) continue;
            if (QDir::cleanPath(source_info.absolutePath()) == QDir::cleanPath(target_dir)) continue;

            QString dest_path = QDir(target_dir).filePath(source_info.fileName());
            if (QFileInfo::exists(dest_path)) {
                const QString base = source_info.completeBaseName();
                const QString suffix = source_info.suffix().isEmpty() ? QString() : QStringLiteral(".") + source_info.suffix();
                int copy = 2;
                do {
                    dest_path = QDir(target_dir).filePath(QStringLiteral("%1 %2%3").arg(base).arg(copy++).arg(suffix));
                } while (QFileInfo::exists(dest_path));
            }

            if (move_template_file(source_path, dest_path))
                moved_any = true;
        }

        if (moved_any && templates_moved)
            templates_moved();
        accept_template_drag(event);
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (auto *item = delete_item_at(event ? event->pos() : QPoint())) {
            setCurrentItem(item);
            if (category_delete_requested)
                category_delete_requested(item);
            return;
        }
        QTreeWidget::mouseReleaseEvent(event);
    }

    void mouseDoubleClickEvent(QMouseEvent *event) override
    {
        if (delete_item_at(event ? event->pos() : QPoint()))
            return;
        QTreeWidget::mouseDoubleClickEvent(event);
    }

private:
    QTreeWidgetItem *delete_item_at(const QPoint &pos) const
    {
        auto *item = itemAt(pos);
        if (!item)
            return nullptr;

        const QRect rect = visualRect(indexFromItem(item, 0));
        const int delete_width = fontMetrics().horizontalAdvance(QStringLiteral("×")) + 12;
        return QRect(rect.left(), rect.top(), delete_width, rect.height()).contains(pos) ? item : nullptr;
    }

    template <typename DragEvent>
    bool accept_template_drag(DragEvent *event)
    {
        if (!event || !event->mimeData() || !event->mimeData()->hasUrls())
            return false;

        if (event->possibleActions() & Qt::MoveAction) {
            event->setDropAction(Qt::MoveAction);
            event->accept();
        } else {
            event->acceptProposedAction();
        }
        return true;
    }

    static bool move_template_file(const QString &source_path, const QString &dest_path)
    {
        if (QFile::rename(source_path, dest_path))
            return true;

        if (!QFile::copy(source_path, dest_path))
            return false;

        if (QFile::remove(source_path))
            return true;

        QFile::remove(dest_path);
        return false;
    }
};

static double title_export_screenshot_time(const Title &title)
{
    const double source_time = title.playback_mode == 2 ? title.pause_time : title.duration * 0.5;
    return std::clamp(source_time, 0.0, std::max(0.0, title.duration));
}

static QImage title_screenshot_image(const Title &title)
{
    return render_title_to_image(title, title_export_screenshot_time(title));
}

static QIcon title_cached_screenshot_icon(const Title &title, const QSize &size)
{
    if (title.preview_screenshot_png_base64.empty())
        return QIcon();

    const QByteArray png = QByteArray::fromBase64(
        QByteArray(title.preview_screenshot_png_base64.data(),
                   (int)title.preview_screenshot_png_base64.size()));
    QPixmap pixmap;
    if (!pixmap.loadFromData(png, "PNG"))
        return QIcon();

    return QIcon(pixmap.scaled(size, Qt::KeepAspectRatio, Qt::SmoothTransformation));
}

static QPixmap title_preview_pixmap(const Title &title, const QSize &size)
{
    QPixmap pixmap;

    if (!title.preview_screenshot_png_base64.empty()) {
        const QByteArray png = QByteArray::fromBase64(
            QByteArray(title.preview_screenshot_png_base64.data(),
                       (int)title.preview_screenshot_png_base64.size()));
        pixmap.loadFromData(png, "PNG");
    }

    if (pixmap.isNull()) {
        const QImage screenshot = title_screenshot_image(title);
        if (!screenshot.isNull())
            pixmap = QPixmap::fromImage(screenshot);
    }

    if (pixmap.isNull())
        return QPixmap();

    return pixmap.scaled(size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
}

static QMessageBox::StandardButton confirm_delete_single_title(QWidget *parent, const Title &title)
{
    QMessageBox box(QMessageBox::Question,
                    obsgs_tr("OBSTitles.DeleteTitle"),
                    obsgs_tr("OBSTitles.DeleteTitleQuestionFormat").arg(QString::fromStdString(title.name)),
                    QMessageBox::Yes | QMessageBox::No,
                    parent);

    const QPixmap preview = title_preview_pixmap(title, QSize(240, 135));
    if (!preview.isNull())
        box.setIconPixmap(preview);

    box.setDefaultButton(QMessageBox::No);
    return static_cast<QMessageBox::StandardButton>(box.exec());
}

static QMessageBox::StandardButton confirm_delete_multiple_titles(QWidget *parent, int count)
{
    return QMessageBox::question(
        parent, obsgs_tr("OBSTitles.DeleteTitle"),
        obsgs_tr("OBSTitles.DeleteSelectedTitlesQuestionFormat").arg(count),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);
}

static QString title_screenshot_png_base64(const QImage &screenshot)
{
    if (screenshot.isNull())
        return QString();

    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    if (!screenshot.save(&buffer, "PNG"))
        return QString();
    return QString::fromLatin1(png.toBase64());
}

static void populate_metadata_from_title(const Title &title, TitleTemplateExportMetadata &metadata)
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
}

static bool prompt_template_metadata(QWidget *parent, const Title &title,
                                     const QImage &screenshot,
                                     TitleTemplateExportMetadata &metadata,
                                     bool *save_in_template_library = nullptr,
                                     const QString &window_title = obsgs_tr("OBSTitles.ExportTemplateDetails"))
{
    populate_metadata_from_title(title, metadata);

    QDialog dialog(parent);
    dialog.setWindowTitle(window_title);
    dialog.setModal(true);
    dialog.resize(560, 500);

    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(obs_layout_spacing(&dialog));

    auto *preview_label = new QLabel(obsgs_tr("OBSTitles.TemplateScreenshotPreviewLabel"), &dialog);
    set_bold_label(preview_label);
    layout->addWidget(preview_label);

    auto *preview = new QLabel(&dialog);
    preview->setAlignment(Qt::AlignCenter);
    preview->setFrameStyle(QFrame::StyledPanel | QFrame::Sunken);
    preview->setMinimumHeight(160);
    if (!screenshot.isNull()) {
        preview->setPixmap(QPixmap::fromImage(screenshot).scaled(
            QSize(480, 180), Qt::KeepAspectRatio, Qt::SmoothTransformation));
    } else if (!metadata.screenshot_png_base64.empty()) {
        QPixmap pixmap;
        const QByteArray png = QByteArray::fromBase64(QByteArray::fromStdString(metadata.screenshot_png_base64));
        if (pixmap.loadFromData(png, "PNG"))
            preview->setPixmap(pixmap.scaled(QSize(480, 180), Qt::KeepAspectRatio, Qt::SmoothTransformation));
        else
            preview->setText(obsgs_tr("OBSTitles.TemplateScreenshotFailed"));
    } else {
        preview->setText(obsgs_tr("OBSTitles.TemplateScreenshotFailed"));
    }
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
    auto validate_and_accept = [&dialog, title_edit, save_in_template_library](bool save_to_library) {
        if (title_edit->text().trimmed().isEmpty()) {
            QMessageBox::warning(&dialog, obsgs_tr("OBSTitles.ExportTemplateDetails"),
                                 obsgs_tr("OBSTitles.TemplateExportTitleRequired"));
            return;
        }
        if (save_in_template_library)
            *save_in_template_library = save_to_library;
        dialog.accept();
    };
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog,
                     [&validate_and_accept]() { validate_and_accept(false); });
    if (save_in_template_library) {
        auto *library_button = buttons->addButton(obsgs_tr("OBSTitles.SaveInTemplateLibrary"), QDialogButtonBox::ActionRole);
        QObject::connect(library_button, &QPushButton::clicked, &dialog,
                         [&validate_and_accept]() { validate_and_accept(true); });
    }
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

struct TemplateLibraryEntry {
    int id;
    const char *name_key;
    const char *description_key;
    const char *default_name_key;
    const char *category_folder;
    const char *file_name;
};

struct TemplateFileMetadata {
    QString title;
    QString description;
    QString creator;
    QString creation_date;
    QIcon screenshot_icon;
    QPixmap screenshot_pixmap;
};

static const std::array<const char *, 5> template_library_category_folders{{
    "Lower Thirds",
    "Tickers",
    "Clocks",
    "Centered",
    "Full Screen",
}};

static const std::array<TemplateLibraryEntry, 5> template_library_entries{{
    {1, "OBSTitles.TemplateLowerThird", "OBSTitles.TemplateLowerThirdDescription", "OBSTitles.TemplateSpeakerName",
     "Lower Thirds", "lower-third.ogspt"},
    {3, "OBSTitles.TemplateTickerStrap", "OBSTitles.TemplateTickerStrapDescription", "OBSTitles.TemplateBreakingNews",
     "Tickers", "ticker-strap.ogspt"},
    {4, "OBSTitles.TemplateClock", "OBSTitles.TemplateClockDescription", "OBSTitles.TemplateClockTitle",
     "Clocks", "clock.ogspt"},
    {2, "OBSTitles.TemplateCenteredTitle", "OBSTitles.TemplateCenteredTitleDescription", "OBSTitles.TemplateProgramTitle",
     "Centered", "centered-title.ogspt"},
    {5, "OBSTitles.TemplateFullScreen", "OBSTitles.TemplateFullScreenDescription", "OBSTitles.TemplateFullScreenTitle",
     "Full Screen", "full-screen.ogspt"},
}};

static const TemplateLibraryEntry *template_library_entry_by_id(int id)
{
    for (const auto &entry : template_library_entries) {
        if (entry.id == id)
            return &entry;
    }
    return nullptr;
}

static QString template_library_root_path()
{
    char *path = obs_module_config_path("template-library");
    QString root = path ? QString::fromUtf8(path) : QDir::homePath() + QStringLiteral("/OBS Graphics Studio Pro Templates");
    bfree(path);
    QDir().mkpath(root);
    return root;
}

static QString template_file_filter()
{
    return QStringLiteral("*.ogspt *.otpt *.json");
}

static TemplateFileMetadata read_template_file_metadata(const QString &path)
{
    TemplateFileMetadata metadata;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        metadata.title = QFileInfo(path).completeBaseName();
        return metadata;
    }

    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    const QJsonObject root = doc.object();
    const QJsonObject meta = root.value(QStringLiteral("metadata")).toObject();
    metadata.title = meta.value(QStringLiteral("title")).toString(
        root.value(QStringLiteral("template_title")).toString(QFileInfo(path).completeBaseName()));
    metadata.description = meta.value(QStringLiteral("description")).toString(root.value(QStringLiteral("description")).toString());
    metadata.creator = meta.value(QStringLiteral("creator")).toString(root.value(QStringLiteral("creator")).toString());
    metadata.creation_date = meta.value(QStringLiteral("creation_date")).toString(root.value(QStringLiteral("creation_date")).toString());

    QJsonObject screenshot = meta.value(QStringLiteral("screenshot")).toObject();
    if (screenshot.isEmpty())
        screenshot = root.value(QStringLiteral("screenshot")).toObject();
    const QByteArray png = QByteArray::fromBase64(screenshot.value(QStringLiteral("data_base64")).toString().toLatin1());
    QPixmap pixmap;
    if (!png.isEmpty() && pixmap.loadFromData(png, "PNG")) {
        metadata.screenshot_pixmap = pixmap;
        metadata.screenshot_icon = QIcon(pixmap.scaled(QSize(96, 54), Qt::KeepAspectRatio, Qt::SmoothTransformation));
    }
    return metadata;
}

static bool write_template_file_metadata(const QString &path, const TemplateFileMetadata &metadata, QString *error = nullptr)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = obsgs_tr("OBSTitles.TemplateEditReadFailed");
        return false;
    }

    QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    file.close();
    if (!doc.isObject()) {
        if (error) *error = obsgs_tr("OBSTitles.TemplateEditReadFailed");
        return false;
    }

    QJsonObject root = doc.object();
    QJsonObject meta = root.value(QStringLiteral("metadata")).toObject();
    meta[QStringLiteral("title")] = metadata.title;
    meta[QStringLiteral("description")] = metadata.description;
    meta[QStringLiteral("creator")] = metadata.creator;
    meta[QStringLiteral("creation_date")] = metadata.creation_date;
    if (!root.value(QStringLiteral("screenshot")).isUndefined())
        meta[QStringLiteral("screenshot")] = root.value(QStringLiteral("screenshot"));

    root[QStringLiteral("template_title")] = metadata.title;
    root[QStringLiteral("description")] = metadata.description;
    root[QStringLiteral("creator")] = metadata.creator;
    root[QStringLiteral("creation_date")] = metadata.creation_date;
    root[QStringLiteral("metadata")] = meta;

    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error) *error = obsgs_tr("OBSTitles.TemplateEditWriteFailed");
        return false;
    }
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    if (!file.flush()) {
        if (error) *error = obsgs_tr("OBSTitles.TemplateEditWriteFailed");
        return false;
    }
    return true;
}

static bool prompt_edit_template_file_metadata(QWidget *parent, const QString &path)
{
    TemplateFileMetadata metadata = read_template_file_metadata(path);

    QDialog dialog(parent);
    dialog.setWindowTitle(obsgs_tr("OBSTitles.EditTemplateMetadata"));
    dialog.setModal(true);
    dialog.resize(520, 360);

    auto *layout = new QVBoxLayout(&dialog);
    auto *form = new QFormLayout();
    auto *title_edit = new QLineEdit(metadata.title, &dialog);
    auto *description_edit = new QTextEdit(&dialog);
    description_edit->setAcceptRichText(false);
    description_edit->setPlainText(metadata.description);
    description_edit->setMinimumHeight(110);
    auto *creator_edit = new QLineEdit(metadata.creator, &dialog);
    auto *date_edit = new QLineEdit(metadata.creation_date, &dialog);

    form->addRow(obsgs_tr("OBSTitles.TemplateExportTitleLabel"), title_edit);
    form->addRow(obsgs_tr("OBSTitles.TemplateExportDescriptionLabel"), description_edit);
    form->addRow(obsgs_tr("OBSTitles.TemplateExportCreatorLabel"), creator_edit);
    form->addRow(obsgs_tr("OBSTitles.TemplateCreationDateLabel"), date_edit);
    layout->addLayout(form);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&]() {
        if (title_edit->text().trimmed().isEmpty()) {
            QMessageBox::warning(&dialog, obsgs_tr("OBSTitles.EditTemplateMetadata"),
                                 obsgs_tr("OBSTitles.TemplateExportTitleRequired"));
            return;
        }
        dialog.accept();
    });
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);

    if (dialog.exec() != QDialog::Accepted)
        return false;

    metadata.title = title_edit->text().trimmed();
    metadata.description = description_edit->toPlainText().trimmed();
    metadata.creator = creator_edit->text().trimmed();
    metadata.creation_date = date_edit->text().trimmed();

    QString error;
    if (!write_template_file_metadata(path, metadata, &error)) {
        QMessageBox::warning(parent, obsgs_tr("OBSTitles.EditTemplateMetadata"), error);
        return false;
    }
    return true;
}

static QTreeWidgetItem *add_template_category_item(QTreeWidget *tree, QTreeWidgetItem *parent,
                                                   const QFileInfo &dir_info)
{
    auto *item = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(tree);
    item->setText(0, QStringLiteral("×  ") + dir_info.fileName());
    item->setToolTip(0, dir_info.absoluteFilePath());
    item->setData(0, Qt::UserRole, dir_info.absoluteFilePath());
    item->setData(0, TemplateCategoryNameRole, dir_info.fileName());
    item->setFlags(item->flags() | Qt::ItemIsDropEnabled);

    QDir dir(dir_info.absoluteFilePath());
    const auto children = dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QFileInfo &child : children)
        add_template_category_item(tree, item, child);
    return item;
}

static void populate_template_categories(QTreeWidget *tree, const QString &root_path)
{
    tree->clear();
    tree->setColumnCount(1);
    tree->setHeaderHidden(true);
    for (const char *folder : template_library_category_folders)
        QDir(root_path).mkpath(QString::fromUtf8(folder));

    QDir root(root_path);
    std::vector<QString> known;
    known.reserve(template_library_category_folders.size());
    for (const char *folder : template_library_category_folders) {
        const QString name = QString::fromUtf8(folder);
        known.push_back(name);
        add_template_category_item(tree, nullptr, QFileInfo(root.filePath(name)));
    }
    for (const QFileInfo &dir : root.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
        if (std::find(known.begin(), known.end(), dir.fileName()) == known.end())
            add_template_category_item(tree, nullptr, dir);
    }

    tree->expandAll();
    if (tree->topLevelItemCount() > 0)
        tree->setCurrentItem(tree->topLevelItem(0));
}

static void collect_template_category_paths(const QDir &root, const QString &relative_dir, QStringList &categories)
{
    const QString scan_path = relative_dir.isEmpty() ? root.absolutePath() : root.filePath(relative_dir);
    QDir dir(scan_path);
    const auto children = dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QFileInfo &child : children) {
        const QString relative_child = relative_dir.isEmpty()
            ? child.fileName()
            : relative_dir + QStringLiteral("/") + child.fileName();
        categories << relative_child;
        collect_template_category_paths(root, relative_child, categories);
    }
}

static QStringList template_library_category_paths(const QString &root_path)
{
    for (const char *folder : template_library_category_folders)
        QDir(root_path).mkpath(QString::fromUtf8(folder));

    QStringList categories;
    QDir root(root_path);
    collect_template_category_paths(root, QString(), categories);
    categories.removeDuplicates();
    categories.sort(Qt::CaseInsensitive);
    return categories;
}

static QString sanitized_template_category_path(QString category)
{
    category.replace('\\', '/');
    QStringList safe_parts;
    for (QString part : category.split('/', Qt::SkipEmptyParts)) {
        part = part.trimmed();
        part.replace(QRegularExpression(QStringLiteral(R"([\\/:*?"<>|])")), QStringLiteral("_"));
        if (!part.isEmpty() && part != QStringLiteral(".") && part != QStringLiteral(".."))
            safe_parts << part;
    }
    return safe_parts.join(QStringLiteral("/"));
}

static bool rename_template_category(QWidget *parent, QTreeWidgetItem *item)
{
    if (!item) return false;

    const QString category_path = item->data(0, Qt::UserRole).toString();
    if (category_path.isEmpty()) return false;

    QFileInfo category_info(category_path);
    bool ok = false;
    const QString current_name = item->data(0, TemplateCategoryNameRole).toString().isEmpty()
        ? category_info.fileName()
        : item->data(0, TemplateCategoryNameRole).toString();
    QString name = QInputDialog::getText(
        parent, obsgs_tr("OBSTitles.RenameCategory"), obsgs_tr("OBSTitles.CategoryNamePrompt"),
        QLineEdit::Normal, current_name, &ok);
    if (!ok) return false;

    name = sanitized_template_category_path(name);
    if (name.isEmpty() || name.contains('/')) {
        QMessageBox::warning(parent, obsgs_tr("OBSTitles.RenameCategory"),
                             obsgs_tr("OBSTitles.TemplateLibraryCategoryRequired"));
        return false;
    }

    const QString parent_path = category_info.dir().absolutePath();
    const QString new_path = QDir(parent_path).filePath(name);
    if (QDir::cleanPath(new_path) == QDir::cleanPath(category_path))
        return false;

    if (QFileInfo::exists(new_path) || !QDir(parent_path).rename(category_info.fileName(), name)) {
        QMessageBox::warning(parent, obsgs_tr("OBSTitles.RenameCategory"),
                             obsgs_tr("OBSTitles.RenameCategoryFailed"));
        return false;
    }

    return true;
}

static bool delete_template_category(QWidget *parent, QTreeWidgetItem *item, const QString &root_path)
{
    if (!item) return false;

    const QString category_path = item->data(0, Qt::UserRole).toString();
    if (category_path.isEmpty()) return false;
    if (QDir::cleanPath(category_path) == QDir::cleanPath(root_path)) return false;

    const auto reply = QMessageBox::question(
        parent, obsgs_tr("OBSTitles.DeleteCategory"),
        obsgs_tr("OBSTitles.DeleteCategoryQuestionFormat").arg(item->data(0, TemplateCategoryNameRole).toString()),
        QMessageBox::Yes | QMessageBox::No);
    if (reply != QMessageBox::Yes) return false;

    if (!QDir(category_path).removeRecursively()) {
        QMessageBox::warning(parent, obsgs_tr("OBSTitles.DeleteCategory"),
                             obsgs_tr("OBSTitles.DeleteCategoryFailed"));
        return false;
    }

    return true;
}

static bool prompt_template_library_category(QWidget *parent, QString &category)
{
    const QString root_path = template_library_root_path();
    QDialog dialog(parent);
    dialog.setWindowTitle(obsgs_tr("OBSTitles.TemplateLibraryCategoryTitle"));
    dialog.setModal(true);

    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(obs_layout_spacing(&dialog));

    auto *prompt = new QLabel(obsgs_tr("OBSTitles.TemplateLibraryCategoryPrompt"), &dialog);
    prompt->setWordWrap(true);
    layout->addWidget(prompt);

    auto *combo = new QComboBox(&dialog);
    combo->setEditable(true);
    combo->addItems(template_library_category_paths(root_path));
    layout->addWidget(combo);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&]() {
        const QString safe_category = sanitized_template_category_path(combo->currentText());
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

} // namespace

/* ══════════════════════════════════════════════════════════════════
 *  Constructor
 * ══════════════════════════════════════════════════════════════════ */
TitleDock::TitleDock(QWidget *parent)
    : QDockWidget(obsgs_tr("OBSTitles.DockName"), parent)
{
    setFeatures(QDockWidget::DockWidgetMovable |
                QDockWidget::DockWidgetFloatable);
    build_ui();

    /* React to external data changes.  Always marshal back to the dock's
     * Qt thread so background/source playback changes cannot touch widgets.
     */
    TitleDataStore::instance().on_change([this]() {
        QTimer::singleShot(0, this, [this]() {
            if (!updating_exposed_text_)
                refresh();
        });
    });

    populate_list();
    seen_store_revision_ = TitleDataStore::instance().revision();
    live_refresh_timer_ = new QTimer(this);
    live_refresh_timer_->setInterval(100);
    connect(live_refresh_timer_, &QTimer::timeout, this, [this]() {
        uint64_t revision = TitleDataStore::instance().revision();
        if (revision == seen_store_revision_ || updating_exposed_text_) return;
        seen_store_revision_ = revision;
        populate_exposed_text();
    });
    live_refresh_timer_->start();
}


void TitleDock::load_dock_settings()
{
    QSettings settings(QStringLiteral("OBSGraphicsStudioPro"), QStringLiteral("Dock"));
    settings.beginGroup(QString::fromUtf8(kDockSettingsGroup));

    template_icon_view_ = settings.value(QString::fromUtf8(kTemplateIconViewKey), template_icon_view_).toBool();
    playlist_loop_ = settings.value(QString::fromUtf8(kPlaylistLoopKey), playlist_loop_).toBool();
    playlist_reverse_ = settings.value(QString::fromUtf8(kPlaylistReverseKey), playlist_reverse_).toBool();
    playlist_hold_seconds_ = std::clamp(settings.value(QString::fromUtf8(kPlaylistHoldSecondsKey),
                                                       playlist_hold_seconds_).toDouble(),
                                        0.0, 3600.0);
    background_persistence_ = settings.value(QString::fromUtf8(kBackgroundPersistenceKey),
                                             background_persistence_).toBool();
    text_persistence_ = settings.value(QString::fromUtf8(kTextPersistenceKey),
                                       text_persistence_).toBool();

    const QByteArray splitter_state = settings.value(QString::fromUtf8(kDockSplitterStateKey)).toByteArray();
    if (!splitter_state.isEmpty() && sections_)
        sections_->restoreState(splitter_state);

    settings.endGroup();
}

void TitleDock::save_dock_settings() const
{
    QSettings settings(QStringLiteral("OBSGraphicsStudioPro"), QStringLiteral("Dock"));
    settings.beginGroup(QString::fromUtf8(kDockSettingsGroup));

    if (sections_)
        settings.setValue(QString::fromUtf8(kDockSplitterStateKey), sections_->saveState());
    settings.setValue(QString::fromUtf8(kTemplateIconViewKey), template_icon_view_);
    settings.setValue(QString::fromUtf8(kPlaylistLoopKey), playlist_loop_);
    settings.setValue(QString::fromUtf8(kPlaylistReverseKey), playlist_reverse_);
    settings.setValue(QString::fromUtf8(kPlaylistHoldSecondsKey), playlist_hold_seconds_);
    settings.setValue(QString::fromUtf8(kBackgroundPersistenceKey), background_persistence_);
    settings.setValue(QString::fromUtf8(kTextPersistenceKey), text_persistence_);

    settings.endGroup();
}

void TitleDock::update_scene_collection_title()
{
    if (template_lbl_)
        template_lbl_->setText(current_scene_collection_titles_label());
}

/* ══════════════════════════════════════════════════════════════════
 *  UI construction
 * ══════════════════════════════════════════════════════════════════ */
void TitleDock::build_ui()
{
    container_ = new QWidget(this);
    auto *root = new QVBoxLayout(container_);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    sections_ = new QSplitter(Qt::Vertical, container_);
    sections_->setChildrenCollapsible(false);
    root->addWidget(sections_, 1);

    auto *template_section = new QWidget(sections_);
    auto *template_layout = new QVBoxLayout(template_section);
    template_layout->setContentsMargins(0, 0, 0, 0);
    template_layout->setSpacing(obs_layout_spacing(template_section));

    /* ── header toolbar ── */
    auto *template_toolbar = make_obs_dock_toolbar(template_section);

    btn_add_ = make_obs_dock_tool_button(template_toolbar, obsgs_tr("OBSTitles.Add"), obs_icon("add.svg"),
                                         obsgs_tr("OBSTitles.AddTooltip"));
    btn_dup_ = make_obs_dock_tool_button(template_toolbar, obsgs_tr("OBSTitles.Duplicate"), obs_icon("duplicate.svg"),
                                         obsgs_tr("OBSTitles.Duplicate"));
    btn_del_ = make_obs_dock_tool_button(template_toolbar, obsgs_tr("OBSTitles.Delete"), obs_icon("delete.svg"),
                                         obsgs_tr("OBSTitles.Delete"));
    btn_rename_ = make_obs_dock_tool_button(template_toolbar, obsgs_tr("OBSTitles.EditDetails"), obs_icon("rename.svg"),
                                            obsgs_tr("OBSTitles.EditDetailsTooltip"));
    btn_export_ = make_obs_dock_tool_button(template_toolbar, obsgs_tr("OBSTitles.Export"), obs_icon("export.svg"),
                                            obsgs_tr("OBSTitles.ExportTooltip"));
    btn_edit_ = make_obs_dock_tool_button(template_toolbar, obsgs_tr("OBSTitles.Edit"), obs_icon("edit.svg"),
                                          obsgs_tr("OBSTitles.EditTooltip"));
    btn_scene_ = make_obs_dock_tool_button(template_toolbar, obsgs_tr("OBSTitles.AddToScene"), obs_icon("add-to-scene.svg"),
                                           obsgs_tr("OBSTitles.AddToSceneTooltip"));
    btn_view_ = make_obs_dock_tool_button(template_toolbar, obsgs_tr("OBSTitles.IconView"), obs_icon("icon-view.svg"),
                                          obsgs_tr("OBSTitles.IconViewTooltip"));

    template_toolbar->addWidget(btn_add_);
    template_toolbar->addSeparator();
    template_toolbar->addWidget(btn_dup_);
    template_toolbar->addWidget(btn_del_);
    template_toolbar->addWidget(toolbar_spacer(template_toolbar));
    template_toolbar->addWidget(btn_view_);
    template_toolbar->addSeparator();
    template_toolbar->addWidget(btn_rename_);
    template_toolbar->addWidget(btn_export_);
    template_toolbar->addWidget(btn_edit_);
    template_toolbar->addWidget(btn_scene_);

    /* ── template/title section ── */
    auto *template_header = new QHBoxLayout();
    template_header->setContentsMargins(0, 0, 0, 0);
    template_header->setSpacing(0);

    template_lbl_ = new QLabel(current_scene_collection_titles_label(), template_section);
    set_bold_label(template_lbl_);
    template_header->addWidget(template_lbl_);
    template_header->addStretch();
    template_layout->addLayout(template_header);
    template_layout->addWidget(template_toolbar);

    list_ = new QListWidget(template_section);
    list_->setAlternatingRowColors(true);
    list_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    list_->setMinimumHeight(120);
    template_layout->addWidget(list_, 1);

    auto *live_section = new QWidget(sections_);
    auto *live_layout = new QVBoxLayout(live_section);
    live_layout->setContentsMargins(0, 0, 0, 0);
    live_layout->setSpacing(obs_layout_spacing(live_section));

    auto *live_header = new QHBoxLayout();
    live_header->setContentsMargins(0, 0, 0, 0);
    live_header->setSpacing(0);

    /* ── exposed text section ── */
    text_editor_lbl_ = new QLabel(obsgs_tr("OBSTitles.LiveText"), live_section);
    set_bold_label(text_editor_lbl_);

    auto *live_toolbar = make_obs_dock_toolbar(live_section);
    btn_add_text_row_ = make_obs_dock_tool_button(live_toolbar, obsgs_tr("OBSTitles.AddRow"), obs_icon("add.svg"),
                                                  obsgs_tr("OBSTitles.AddCueRowTooltip"));
    btn_delete_text_row_ = make_obs_dock_tool_button(live_toolbar, obsgs_tr("OBSTitles.Delete"), obs_icon("delete.svg"),
                                                     obsgs_tr("OBSTitles.DeleteCueTooltip"));
    btn_row_up_ = make_obs_dock_tool_button(live_toolbar, obsgs_tr("OBSTitles.MoveUp"), obs_icon("move-up.svg"),
                                            obsgs_tr("OBSTitles.MoveCueUpTooltip"));
    btn_row_down_ = make_obs_dock_tool_button(live_toolbar, obsgs_tr("OBSTitles.MoveDown"), obs_icon("move-down.svg"),
                                              obsgs_tr("OBSTitles.MoveCueDownTooltip"));
    btn_data_sources_ = make_obs_dock_tool_button(live_toolbar, obsgs_tr("OBSTitles.DataSources"),
                                                  obs_icon("data-sources.svg"),
                                                  obsgs_tr("OBSTitles.DataSourcesTooltip"));
    btn_data_sources_->setText(obsgs_tr("OBSTitles.DataSources"));
    btn_data_sources_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    auto *data_sources_menu = new QMenu(btn_data_sources_);
    data_sources_menu->addAction(obs_icon("import.svg"), obsgs_tr("OBSTitles.ImportData"),
                                 this, &TitleDock::on_import_live_text_data);
    data_sources_menu->addAction(obs_icon("import.svg"), obsgs_tr("OBSTitles.ImportAppendData"),
                                 this, &TitleDock::on_import_append_live_text_data);
    data_sources_menu->addAction(obs_icon("export.svg"), obsgs_tr("OBSTitles.ExportData"),
                                 this, &TitleDock::on_export_live_text_data);
    data_sources_menu->addSeparator();
    data_sources_menu->addAction(obsgs_tr("OBSTitles.EnableExternalDataSource"),
                                 this, &TitleDock::on_toggle_external_data_source);
    data_sources_menu->addAction(obsgs_tr("OBSTitles.Settings"),
                                 this, &TitleDock::on_show_external_data_settings);
    btn_data_sources_->setMenu(data_sources_menu);
    btn_data_sources_->setPopupMode(QToolButton::InstantPopup);
    btn_data_sources_->setStyleSheet(QStringLiteral("QToolButton::menu-indicator{image:none;width:0px;}"));
    btn_external_refresh_ = make_obs_dock_tool_button(live_toolbar, obsgs_tr("OBSTitles.RefreshExternalData"),
                                                      globe_status_icon(false, live_toolbar),
                                                      obsgs_tr("OBSTitles.RefreshExternalDataTooltip"));
    btn_external_refresh_->setCheckable(true);
    btn_external_refresh_->setMinimumWidth(obs_toolbar_icon_extent(live_toolbar) + 10);
    btn_external_refresh_->setStyleSheet(QStringLiteral(
        "QToolButton:checked{background:#1d8f3a;color:white;border-radius:3px;}"
        "QToolButton:checked:hover{background:#28b84f;}"));
    btn_playlist_ = make_obs_dock_tool_button(live_toolbar, obsgs_tr("OBSTitles.Playlist"), obs_icon("play.svg"),
                                              obsgs_tr("OBSTitles.PlaylistTooltip"));
    btn_playlist_->setCheckable(true);
    btn_playlist_->setStyleSheet(QStringLiteral(
        "QToolButton:checked{background:#1d8f3a;color:white;border-radius:3px;}"
        "QToolButton:checked:hover{background:#28b84f;}"));
    playlist_countdown_lbl_ = new QLabel(QStringLiteral("--"), live_toolbar);
    playlist_countdown_lbl_->setToolTip(obsgs_tr("OBSTitles.PlaylistNextCueTooltip"));
    playlist_countdown_lbl_->setMinimumWidth(44);
    playlist_countdown_lbl_->setAlignment(Qt::AlignCenter);
    btn_playlist_settings_ = make_obs_dock_tool_button(live_toolbar, obsgs_tr("OBSTitles.PlaylistSettings"), QIcon(),
                                                       obsgs_tr("OBSTitles.PlaylistSettingsTooltip"));
    btn_playlist_settings_->setText(QStringLiteral("⚙"));
    btn_playlist_settings_->setToolButtonStyle(Qt::ToolButtonTextOnly);
    btn_persistence_settings_ = make_obs_dock_tool_button(live_toolbar, obsgs_tr("OBSTitles.Persistence"),
                                                          obs_icon("persistence.svg"),
                                                          obsgs_tr("OBSTitles.PersistenceTooltip"));
    btn_persistence_settings_->setToolButtonStyle(Qt::ToolButtonIconOnly);
    btn_persistence_settings_->setMinimumWidth(obs_toolbar_icon_extent(live_toolbar) + 10);
    btn_persistence_settings_->setCheckable(true);
    btn_persistence_settings_->setStyleSheet(QStringLiteral(
        "QToolButton:checked{background:#1d8f3a;color:white;border-radius:3px;}"
        "QToolButton:checked:hover{background:#28b84f;}"
        "QToolButton::menu-indicator{image:none;width:0px;}"));
    live_toolbar->addWidget(btn_add_text_row_);
    live_toolbar->addWidget(btn_delete_text_row_);
    live_toolbar->addWidget(btn_row_up_);
    live_toolbar->addWidget(btn_row_down_);
    live_toolbar->addWidget(btn_data_sources_);
    live_toolbar->addSeparator();
    live_toolbar->addWidget(btn_playlist_);
    live_toolbar->addWidget(playlist_countdown_lbl_);
    live_toolbar->addWidget(btn_playlist_settings_);
    live_toolbar->addWidget(btn_persistence_settings_);
    live_toolbar->addWidget(toolbar_spacer(live_toolbar));
    live_toolbar->addWidget(btn_external_refresh_);

    live_header->addWidget(text_editor_lbl_);
    live_header->addStretch();
    live_layout->addLayout(live_header);

    text_table_ = new LiveTextCueTable(live_section);
    auto *live_text_header = new LiveTextCueHeader(text_table_);
    live_text_header->select_all_toggled = [this](bool checked) { set_all_live_text_rows_checked(checked); };
    text_table_->setHorizontalHeader(live_text_header);
    text_table_->setMinimumHeight(96);
    text_table_->setAlternatingRowColors(false);
    text_table_->verticalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    text_table_->verticalHeader()->setDefaultSectionSize(30);
    text_table_->horizontalHeader()->setStretchLastSection(false);
    text_table_->horizontalHeader()->setSectionsMovable(true);
    text_table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    text_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    text_table_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    text_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    live_layout->addWidget(text_table_, 1);
    live_layout->addWidget(live_toolbar);

    sections_->addWidget(template_section);
    sections_->addWidget(live_section);
    sections_->setStretchFactor(0, 2);
    sections_->setStretchFactor(1, 1);

    /* ── status ── */
    status_lbl_ = new QLabel(obsgs_tr("OBSTitles.NoTitleSelected"), container_);
    status_lbl_->setAlignment(Qt::AlignCenter);
    QFont sf = status_lbl_->font();
    sf.setPointSize(std::max(1, sf.pointSize() - 1));
    status_lbl_->setFont(sf);
    template_layout->addWidget(status_lbl_);
    template_layout->addWidget(template_toolbar);
    update_template_view_mode();

    setWidget(container_);

    /* ── connections ── */
    auto *add_menu = new QMenu(btn_add_);
    add_menu->addAction(obsgs_tr("OBSTitles.AddBlankTitle"), this, &TitleDock::on_add);
    add_menu->addAction(obsgs_tr("OBSTitles.AddFromTemplatesLibrary"), this, &TitleDock::on_add_from_templates_library);
    add_menu->addAction(obsgs_tr("OBSTitles.Import"), this, &TitleDock::on_import);
    btn_add_->setMenu(add_menu);
    btn_add_->setPopupMode(QToolButton::InstantPopup);
    btn_add_->setStyleSheet(QStringLiteral("QToolButton::menu-indicator{image:none;width:0px;}"));

    connect(btn_dup_,   &QToolButton::clicked, this, &TitleDock::on_duplicate);
    connect(btn_rename_, &QToolButton::clicked, this, &TitleDock::on_rename);
    connect(btn_del_,   &QToolButton::clicked, this, &TitleDock::on_delete);
    connect(btn_export_, &QToolButton::clicked, this, &TitleDock::on_export);
    connect(btn_edit_,  &QToolButton::clicked, this, &TitleDock::on_edit);
    connect(btn_scene_, &QToolButton::clicked, this, &TitleDock::on_add_to_scene);
    connect(btn_view_, &QToolButton::clicked, this, &TitleDock::on_toggle_template_view);
    connect(btn_add_text_row_, &QToolButton::clicked, this, &TitleDock::on_add_live_text_row);
    connect(btn_delete_text_row_, &QToolButton::clicked, this, &TitleDock::on_delete_live_text_rows);
    connect(btn_row_up_, &QToolButton::clicked, this, &TitleDock::on_move_live_text_row_up);
    auto *playlist_menu = new QMenu(btn_playlist_settings_);
    act_playlist_loop_ = playlist_menu->addAction(obsgs_tr("OBSTitles.PlaylistLoop"));
    act_playlist_loop_->setCheckable(true);
    act_playlist_reverse_ = playlist_menu->addAction(obsgs_tr("OBSTitles.PlaylistReverseOrder"));
    act_playlist_reverse_->setCheckable(true);
    playlist_menu->addSeparator();
    auto *hold_widget = new QWidget(playlist_menu);
    auto *hold_layout = new QHBoxLayout(hold_widget);
    hold_layout->setContentsMargins(8, 4, 8, 4);
    hold_layout->addWidget(new QLabel(obsgs_tr("OBSTitles.PlaylistHoldSeconds"), hold_widget));
    auto *hold_spin = new QDoubleSpinBox(hold_widget);
    hold_spin->setRange(0.0, 3600.0);
    hold_spin->setDecimals(1);
    hold_spin->setSingleStep(0.5);
    hold_spin->setSuffix(QStringLiteral(" s"));
    hold_spin->setValue(playlist_hold_seconds_);
    hold_layout->addWidget(hold_spin);
    auto *hold_action = new QWidgetAction(playlist_menu);
    hold_action->setDefaultWidget(hold_widget);
    playlist_menu->addAction(hold_action);
    btn_playlist_settings_->setMenu(playlist_menu);
    btn_playlist_settings_->setPopupMode(QToolButton::InstantPopup);
    btn_playlist_settings_->setStyleSheet(QStringLiteral("QToolButton::menu-indicator{image:none;width:0px;}"));

    auto *persistence_menu = new QMenu(btn_persistence_settings_);
    act_background_persistence_ = persistence_menu->addAction(obsgs_tr("OBSTitles.BackgroundPersistence"));
    act_background_persistence_->setCheckable(true);
    act_text_persistence_ = persistence_menu->addAction(obsgs_tr("OBSTitles.TextPersistence"));
    act_text_persistence_->setCheckable(true);
    btn_persistence_settings_->setMenu(persistence_menu);
    btn_persistence_settings_->setPopupMode(QToolButton::InstantPopup);

    connect(btn_row_down_, &QToolButton::clicked, this, &TitleDock::on_move_live_text_row_down);
    connect(btn_external_refresh_, &QToolButton::clicked, this, &TitleDock::on_refresh_external_data);
    connect(btn_playlist_, &QToolButton::toggled, this, &TitleDock::on_toggle_playlist);
    connect(act_playlist_loop_, &QAction::toggled, this, [this](bool checked) {
        playlist_loop_ = checked;
        save_dock_settings();
    });
    connect(act_playlist_reverse_, &QAction::toggled, this, [this](bool checked) {
        playlist_reverse_ = checked;
        save_dock_settings();
    });
    connect(hold_spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double value) {
        playlist_hold_seconds_ = value;
        save_dock_settings();
    });
    connect(act_background_persistence_, &QAction::toggled, this, [this](bool checked) {
        background_persistence_ = checked;
        if (!background_persistence_) {
            text_persistence_ = false;
            if (act_text_persistence_) {
                QSignalBlocker block(act_text_persistence_);
                act_text_persistence_->setChecked(false);
            }
        }
        if (auto title = TitleDataStore::instance().get_title(selected_id()))
            apply_persistence_settings_to_title(title);
        update_persistence_controls();
        save_dock_settings();
    });
    connect(act_text_persistence_, &QAction::toggled, this, [this](bool checked) {
        text_persistence_ = background_persistence_ && checked;
        if (act_text_persistence_ && act_text_persistence_->isChecked() != text_persistence_) {
            QSignalBlocker block(act_text_persistence_);
            act_text_persistence_->setChecked(text_persistence_);
        }
        if (auto title = TitleDataStore::instance().get_title(selected_id()))
            apply_persistence_settings_to_title(title);
        save_dock_settings();
    });
    connect(text_table_, &QTableWidget::itemChanged, this, [this](QTableWidgetItem *item) {
        if (item && item->column() == 0)
            update_live_text_select_all_state();
    });
    connect(sections_, &QSplitter::splitterMoved, this, [this](int, int) { save_dock_settings(); });
    connect(text_table_->horizontalHeader(), &QHeaderView::sectionMoved,
            this, [this](int, int, int) { save_live_text_header_state(); });
    connect(text_table_->horizontalHeader(), &QHeaderView::sectionResized,
            this, [this](int, int, int) { save_live_text_header_state(); });
    connect(list_, &QListWidget::itemSelectionChanged,
            this, &TitleDock::on_selection_changed);
    connect(list_, &QListWidget::itemDoubleClicked,
            this, [this](QListWidgetItem *) { on_edit(); });

    playlist_timer_ = new QTimer(this);
    playlist_timer_->setInterval(250);
    connect(playlist_timer_, &QTimer::timeout, this, &TitleDock::on_playlist_tick);

    load_dock_settings();
    if (act_playlist_loop_) act_playlist_loop_->setChecked(playlist_loop_);
    if (act_playlist_reverse_) act_playlist_reverse_->setChecked(playlist_reverse_);
    if (act_background_persistence_) act_background_persistence_->setChecked(background_persistence_);
    if (act_text_persistence_) act_text_persistence_->setChecked(text_persistence_);
    hold_spin->setValue(playlist_hold_seconds_);
    update_template_view_mode();
    update_playlist_controls();
    update_persistence_controls();
    update_playlist_countdown_label();
    on_selection_changed();
}

/* ══════════════════════════════════════════════════════════════════
 *  List population
 * ══════════════════════════════════════════════════════════════════ */
void TitleDock::update_template_view_mode()
{
    if (!list_) return;

    if (template_icon_view_) {
        list_->setViewMode(QListView::IconMode);
        list_->setIconSize(QSize(120, 72));
        list_->setResizeMode(QListView::Adjust);
        list_->setMovement(QListView::Static);
        list_->setSpacing(8);
        list_->setUniformItemSizes(false);
        if (btn_view_) {
            btn_view_->setText(obsgs_tr("OBSTitles.ListView"));
            btn_view_->setAccessibleName(obsgs_tr("OBSTitles.ListView"));
            btn_view_->setToolTip(obsgs_tr("OBSTitles.ListViewTooltip"));
            btn_view_->setIcon(obs_icon("list-view.svg"));
        }
    } else {
        list_->setViewMode(QListView::ListMode);
        list_->setIconSize(QSize());
        list_->setResizeMode(QListView::Fixed);
        list_->setMovement(QListView::Static);
        list_->setSpacing(0);
        list_->setUniformItemSizes(true);
        if (btn_view_) {
            btn_view_->setText(obsgs_tr("OBSTitles.IconView"));
            btn_view_->setAccessibleName(obsgs_tr("OBSTitles.IconView"));
            btn_view_->setToolTip(obsgs_tr("OBSTitles.IconViewTooltip"));
            btn_view_->setIcon(obs_icon("icon-view.svg"));
        }
    }
}

void TitleDock::on_toggle_template_view()
{
    template_icon_view_ = !template_icon_view_;
    update_template_view_mode();
    populate_list();
    save_dock_settings();
}

void TitleDock::populate_list()
{
    QString prev_id = QString::fromStdString(selected_id());
    list_->blockSignals(true);
    list_->clear();

    for (auto &t : TitleDataStore::instance().titles()) {
        auto *item = new QListWidgetItem(QString::fromStdString(t->name));
        if (template_icon_view_)
            item->setIcon(title_cached_screenshot_icon(*t, QSize(120, 72)));
        item->setData(Qt::UserRole, QString::fromStdString(t->id));
        // Layer count hint as tooltip
        item->setToolTip(
            obsgs_tr("OBSTitles.LayerCountTooltipFormat").arg(t->layers.size()).arg(t->duration));
        list_->addItem(item);
    }

    /* Restore selection */
    for (int i = 0; i < list_->count(); ++i) {
        if (list_->item(i)->data(Qt::UserRole).toString() == prev_id) {
            list_->setCurrentRow(i);
            break;
        }
    }

    list_->blockSignals(false);
    on_selection_changed();
}

void TitleDock::refresh()
{
    populate_list();
    populate_exposed_text();
}

/* ══════════════════════════════════════════════════════════════════
 *  Selection helper
 * ══════════════════════════════════════════════════════════════════ */
std::string TitleDock::selected_id() const
{
    auto *item = list_->currentItem();
    if (!item) return {};
    return item->data(Qt::UserRole).toString().toStdString();
}

std::vector<std::string> TitleDock::selected_title_ids() const
{
    std::vector<std::string> ids;
    if (!list_) return ids;

    const auto items = list_->selectedItems();
    ids.reserve((size_t)items.size());
    for (const auto *item : items) {
        if (!item) continue;
        const std::string id = item->data(Qt::UserRole).toString().toStdString();
        if (!id.empty())
            ids.push_back(id);
    }

    return ids;
}

void TitleDock::on_selection_changed()
{
    const auto ids = selected_title_ids();
    const int selected_count = (int)ids.size();
    const bool has = selected_count > 0;
    const bool single = selected_count == 1;

    btn_dup_->setEnabled(single);
    btn_rename_->setEnabled(single);
    btn_del_->setEnabled(has);
    btn_export_->setEnabled(single);
    btn_edit_->setEnabled(single);
    btn_scene_->setEnabled(single);

    if (single) {
        auto t = TitleDataStore::instance().get_title(ids.front());
        if (t)
            status_lbl_->setText(
                obsgs_tr("OBSTitles.StatusLayerCountFormat")
                    .arg(t->layers.size())
                    .arg(t->duration, 0, 'f', 1));
    } else if (has) {
        status_lbl_->setText(obsgs_tr("OBSTitles.SelectedTitlesStatusFormat").arg(selected_count));
    } else {
        status_lbl_->setText(list_->count() == 0
            ? obsgs_tr("OBSTitles.UseAddHint")
            : obsgs_tr("OBSTitles.NoTitleSelected"));
    }
    populate_exposed_text();
}

void TitleDock::save_live_text_header_state()
{
    if (!text_table_ || text_table_->columnCount() <= 0) return;
    const QByteArray state = text_table_->horizontalHeader()->saveState();
    live_text_header_states_[text_table_->columnCount()] = state;

    auto title = TitleDataStore::instance().get_title(selected_id());
    if (!title) return;
    const QByteArray encoded = state.toBase64();
    title->live_text_header_state.assign(encoded.constData(), (size_t)encoded.size());
    TitleDataStore::instance().save();
}

bool TitleDock::restore_live_text_header_state()
{
    if (!text_table_ || text_table_->columnCount() <= 0) return false;

    auto title = TitleDataStore::instance().get_title(selected_id());
    if (title && !title->live_text_header_state.empty()) {
        const QByteArray encoded(title->live_text_header_state.data(), (int)title->live_text_header_state.size());
        const QByteArray state = QByteArray::fromBase64(encoded);
        if (!state.isEmpty() && text_table_->horizontalHeader()->restoreState(state)) {
            live_text_header_states_[text_table_->columnCount()] = state;
            return true;
        }
    }

    auto it = live_text_header_states_.find(text_table_->columnCount());
    if (it == live_text_header_states_.end()) return false;
    return text_table_->horizontalHeader()->restoreState(it->second);
}

bool TitleDock::has_checked_live_text_rows() const
{
    if (!text_table_) return false;
    for (int row = 0; row < text_table_->rowCount(); ++row) {
        auto *item = text_table_->item(row, 0);
        if (item && item->checkState() == Qt::Checked)
            return true;
    }
    return false;
}

void TitleDock::apply_live_text_row_selection(const std::vector<int> &rows, bool checked)
{
    if (!text_table_) return;

    QSignalBlocker block(text_table_);
    text_table_->clearSelection();
    for (int row = 0; row < text_table_->rowCount(); ++row) {
        auto *item = text_table_->item(row, 0);
        if (item)
            item->setCheckState(Qt::Unchecked);
    }

    auto *selection_model = text_table_->selectionModel();
    for (int row : rows) {
        if (row < 0 || row >= text_table_->rowCount()) continue;
        if (checked) {
            auto *item = text_table_->item(row, 0);
            if (item)
                item->setCheckState(Qt::Checked);
        }
        if (selection_model && text_table_->columnCount() > 0) {
            const QModelIndex left = text_table_->model()->index(row, 0);
            const QModelIndex right = text_table_->model()->index(row, text_table_->columnCount() - 1);
            selection_model->select(QItemSelection(left, right),
                                    QItemSelectionModel::Select | QItemSelectionModel::Rows);
        }
    }
    if (!rows.empty() && selection_model)
        selection_model->setCurrentIndex(text_table_->model()->index(rows.front(), 0), QItemSelectionModel::NoUpdate);
    update_live_text_select_all_state();
}

void TitleDock::set_all_live_text_rows_checked(bool checked)
{
    if (!text_table_) return;

    QSignalBlocker block(text_table_);
    for (int row = 0; row < text_table_->rowCount(); ++row) {
        auto *item = text_table_->item(row, 0);
        if (item)
            item->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
    }
    update_live_text_select_all_state();
}

void TitleDock::update_live_text_select_all_state()
{
    auto *header = live_text_cue_header(text_table_);
    if (!header || !text_table_) return;

    const int row_count = text_table_->rowCount();
    bool all_checked = row_count > 0;
    for (int row = 0; row < row_count; ++row) {
        auto *item = text_table_->item(row, 0);
        if (!item || item->checkState() != Qt::Checked) {
            all_checked = false;
            break;
        }
    }
    header->set_select_all_checked(all_checked);
}

std::vector<int> TitleDock::selected_live_text_rows() const
{
    std::vector<int> rows;
    if (!text_table_) return rows;

    for (int row = 0; row < text_table_->rowCount(); ++row) {
        auto *item = text_table_->item(row, 0);
        if (item && item->checkState() == Qt::Checked)
            rows.push_back(row);
    }

    if (rows.empty()) {
        for (const auto *item : text_table_->selectedItems()) {
            if (!item) continue;
            int row = item->row();
            if (std::find(rows.begin(), rows.end(), row) == rows.end())
                rows.push_back(row);
        }
    }

    std::sort(rows.begin(), rows.end());
    return rows;
}

void TitleDock::apply_persistence_settings_to_title(const std::shared_ptr<Title> &title)
{
    if (!title) return;
    auto exposed = exposed_text_layers(title);
    const bool has_exposed = !exposed.empty();
    title->cue_background_persistence = background_persistence_ && has_exposed;
    title->cue_text_persistence = title->cue_background_persistence && text_persistence_;
    if (!title->cue_background_persistence)
        title->cue_persistence_transition = false;
    if (!title->cue_text_persistence)
        title->cue_persistent_text_columns.clear();
}

void TitleDock::update_persistence_controls()
{
    auto title = TitleDataStore::instance().get_title(selected_id());
    const bool has_exposed = title && !exposed_text_layers(title).empty();
    if (btn_persistence_settings_) {
        btn_persistence_settings_->setEnabled(has_exposed);
        QSignalBlocker block(btn_persistence_settings_);
        btn_persistence_settings_->setChecked(has_exposed && background_persistence_);
    }
    if (act_background_persistence_)
        act_background_persistence_->setEnabled(has_exposed);
    if (act_text_persistence_) {
        act_text_persistence_->setEnabled(has_exposed && background_persistence_);
        if (!background_persistence_ && act_text_persistence_->isChecked()) {
            QSignalBlocker block(act_text_persistence_);
            act_text_persistence_->setChecked(false);
        }
    }
    if (title)
        apply_persistence_settings_to_title(title);
}

void TitleDock::update_external_data_controls()
{
    auto title = TitleDataStore::instance().get_title(selected_id());
    const bool has_title = (bool)title;
    const bool has_exposed = title && !exposed_text_layers(title).empty();
    const bool external_enabled = title && title->external_data_enabled;

    if (btn_data_sources_)
        btn_data_sources_->setEnabled(has_title && has_exposed);
    if (btn_external_refresh_) {
        btn_external_refresh_->setEnabled(has_title && has_exposed);
        btn_external_refresh_->setIcon(globe_status_icon(external_enabled, btn_external_refresh_));
        QSignalBlocker block(btn_external_refresh_);
        btn_external_refresh_->setChecked(external_enabled);
        btn_external_refresh_->setToolTip(external_enabled
            ? obsgs_tr("OBSTitles.RefreshExternalDataEnabledTooltip")
            : obsgs_tr("OBSTitles.RefreshExternalDataTooltip"));
    }

    if (btn_data_sources_ && btn_data_sources_->menu() && btn_data_sources_->menu()->actions().size() >= 5) {
        auto actions = btn_data_sources_->menu()->actions();
        for (QAction *action : actions)
            action->setEnabled(has_title && has_exposed);
        if (QAction *toggle = actions.at(4))
            toggle->setText(external_enabled
                ? obsgs_tr("OBSTitles.DisableExternalDataSource")
                : obsgs_tr("OBSTitles.EnableExternalDataSource"));
    }
}

bool TitleDock::cue_live_text_row(int row, bool allow_uncue)
{
    auto title = TitleDataStore::instance().get_title(selected_id());
    if (!title) return false;

    auto exposed_now = exposed_text_layers(title);
    normalize_live_text_rows(title, exposed_now);

    if (exposed_now.empty()) {
        updating_exposed_text_ = true;
        title->current_cue_row = -1;
        title->pending_cue_row = -1;
        ++title->cue_revision;
        TitleDataStore::instance().save();
        TitleDataStore::instance().notify_change();
        updating_exposed_text_ = false;
        populate_exposed_text();
        return true;
    }

    if (row < 0 || row >= (int)title->live_text_rows.size())
        return false;

    updating_exposed_text_ = true;
    apply_persistence_settings_to_title(title);
    const bool is_active_cue = title->current_cue_row == row;
    const bool is_pending_cue = title->pending_cue_row == row;
    const int previous_row = title->current_cue_row >= 0 ? title->current_cue_row : title->pending_cue_row;
    const bool can_persist_transition = title->cue_background_persistence &&
        (title->playback_mode == 1 || title->playback_mode == 2) &&
        previous_row >= 0 && previous_row != row;
    const bool needs_outro_before_cue =
        (title->playback_mode == 1 || title->playback_mode == 2) &&
        title->current_cue_row >= 0 && title->current_cue_row != row;

    title->cue_persistence_transition = false;
    title->cue_persistent_text_columns.assign(exposed_now.size(), false);

    if (allow_uncue && (is_active_cue || is_pending_cue)) {
        title->current_cue_row = -1;
        title->pending_cue_row = -1;
        title->cue_persistence_transition = false;
        title->cue_persistent_text_columns.clear();
    } else if (can_persist_transition) {
        for (int col = 0; col < (int)exposed_now.size() && col < (int)title->live_text_rows[row].size(); ++col) {
            if (title->cue_text_persistence &&
                previous_row >= 0 && previous_row < (int)title->live_text_rows.size() &&
                col < (int)title->live_text_rows[previous_row].size() &&
                title->live_text_rows[previous_row][col] == title->live_text_rows[row][col])
                title->cue_persistent_text_columns[col] = true;
        }
        title->pending_cue_row = row;
        title->cue_persistence_transition = true;
    } else if (needs_outro_before_cue) {
        title->pending_cue_row = row;
    } else if (!is_active_cue || title->pending_cue_row >= 0) {
        for (int col = 0; col < (int)exposed_now.size() && col < (int)title->live_text_rows[row].size(); ++col)
            exposed_now[col]->text_content = title->live_text_rows[row][col];
        title->current_cue_row = row;
        title->pending_cue_row = -1;
    }

    ++title->cue_revision;
    TitleDataStore::instance().save();
    TitleDataStore::instance().notify_change();
    updating_exposed_text_ = false;
    populate_exposed_text();
    return true;
}

int TitleDock::live_text_playlist_row_count(const std::shared_ptr<Title> &title) const
{
    if (!title) return 0;
    auto exposed = exposed_text_layers(title);
    if (exposed.empty())
        return 1;
    return (int)title->live_text_rows.size();
}

int TitleDock::next_playlist_row(int current_row, int row_count) const
{
    if (row_count <= 0) return 0;
    return (current_row + (playlist_reverse_ ? -1 : 1) + row_count) % row_count;
}

int TitleDock::playlist_step_delay_ms(const std::shared_ptr<Title> &title) const
{
    if (!title) return 1000;

    double seconds = playlist_hold_seconds_;
    if (title->playback_mode == 1)
        seconds += std::clamp(title->loop_end, title->loop_start, title->duration);
    else if (title->playback_mode == 2)
        seconds += std::clamp(title->pause_time, 0.0, title->duration);
    else
        seconds += title->duration;

    return std::max(1, (int)std::round(seconds * 1000.0));
}

int TitleDock::playlist_hold_delay_ms() const
{
    return std::max(1, (int)std::round(playlist_hold_seconds_ * 1000.0));
}

bool TitleDock::playlist_row_is_terminal(int row, int row_count) const
{
    if (row < 0 || row_count <= 0) return false;
    return playlist_reverse_ ? row == 0 : row == row_count - 1;
}

void TitleDock::play_playlist_outro()
{
    auto title = TitleDataStore::instance().get_title(selected_id());
    if (!title) return;

    const int row = title->pending_cue_row >= 0 ? title->pending_cue_row : title->current_cue_row;
    if (row >= 0)
        cue_live_text_row(row, true);
    else if (live_text_playlist_row_count(title) == 1)
        cue_live_text_row(0, true);
}

void TitleDock::stop_playlist()
{
    if (playlist_timer_)
        playlist_timer_->stop();
    playlist_next_due_ms_ = 0;
    playlist_stop_after_due_ = false;
    if (btn_playlist_ && btn_playlist_->isChecked()) {
        QSignalBlocker block(btn_playlist_);
        btn_playlist_->setChecked(false);
    }
    update_playlist_countdown_label();
}

void TitleDock::update_playlist_countdown_label()
{
    if (!playlist_countdown_lbl_) return;

    const bool active = btn_playlist_ && btn_playlist_->isChecked() && playlist_next_due_ms_ > 0;
    if (!active) {
        playlist_countdown_lbl_->setText(QStringLiteral("--"));
        playlist_countdown_lbl_->setVisible(false);
        return;
    }

    const qint64 remaining_ms = std::max<qint64>(0, playlist_next_due_ms_ - QDateTime::currentMSecsSinceEpoch());
    const double remaining_seconds = remaining_ms / 1000.0;
    playlist_countdown_lbl_->setText(QStringLiteral("%1s").arg(remaining_seconds, 0, 'f', remaining_seconds < 10.0 ? 1 : 0));
    playlist_countdown_lbl_->setVisible(true);
}

void TitleDock::start_playlist_step()
{
    auto title = TitleDataStore::instance().get_title(selected_id());
    const int row_count = live_text_playlist_row_count(title);
    if (!title || row_count <= 0) {
        stop_playlist();
        return;
    }

    if (playlist_next_row_ < 0 || playlist_next_row_ >= row_count)
        playlist_next_row_ = playlist_reverse_ ? row_count - 1 : 0;

    const int row = playlist_next_row_;
    const bool already_active = title->pending_cue_row < 0 && title->current_cue_row == row;
    if (!already_active)
        cue_live_text_row(row, false);
    playlist_next_row_ = next_playlist_row(row, row_count);
    playlist_stop_after_due_ = !playlist_loop_ && playlist_row_is_terminal(row, row_count);

    playlist_next_due_ms_ = QDateTime::currentMSecsSinceEpoch() +
        (already_active ? playlist_hold_delay_ms() : playlist_step_delay_ms(title));
    if (playlist_timer_ && !playlist_timer_->isActive())
        playlist_timer_->start();
    update_playlist_countdown_label();
}

void TitleDock::on_playlist_tick()
{
    if (!btn_playlist_ || !btn_playlist_->isChecked()) return;
    if (QDateTime::currentMSecsSinceEpoch() >= playlist_next_due_ms_) {
        if (playlist_stop_after_due_) {
            play_playlist_outro();
            stop_playlist();
        } else {
            start_playlist_step();
        }
    } else {
        update_playlist_countdown_label();
    }
}

void TitleDock::on_toggle_playlist(bool enabled)
{
    if (!enabled) {
        stop_playlist();
        return;
    }

    auto title = TitleDataStore::instance().get_title(selected_id());
    const int row_count = live_text_playlist_row_count(title);
    if (!title || row_count <= 0) {
        stop_playlist();
        return;
    }

    int base = title->pending_cue_row >= 0 ? title->pending_cue_row : title->current_cue_row;
    if (title->pending_cue_row < 0 && base >= 0 && base < row_count) {
        playlist_next_row_ = next_playlist_row(base, row_count);
        playlist_stop_after_due_ = !playlist_loop_ && playlist_row_is_terminal(base, row_count);
        playlist_next_due_ms_ = QDateTime::currentMSecsSinceEpoch() + playlist_hold_delay_ms();
        if (playlist_timer_ && !playlist_timer_->isActive())
            playlist_timer_->start();
        update_playlist_countdown_label();
        return;
    }

    if (base >= 0 && base < row_count)
        playlist_next_row_ = base;
    else
        playlist_next_row_ = playlist_reverse_ ? row_count - 1 : 0;

    start_playlist_step();
}

void TitleDock::update_playlist_controls()
{
    auto title = TitleDataStore::instance().get_title(selected_id());
    const bool enabled = title && live_text_playlist_row_count(title) > 0;
    if (btn_playlist_) {
        btn_playlist_->setEnabled(enabled);
        if (!enabled && btn_playlist_->isChecked())
            stop_playlist();
    }
    if (btn_playlist_settings_)
        btn_playlist_settings_->setEnabled(enabled);
    update_playlist_countdown_label();
}

void TitleDock::populate_exposed_text()
{
    if (!text_table_) return;
    QSignalBlocker block(text_table_);
    QSignalBlocker header_block(text_table_->horizontalHeader());
    text_table_->clear();
    text_table_->setRowCount(0);
    text_table_->setColumnCount(0);

    auto *header = live_text_cue_header(text_table_);

    auto title = TitleDataStore::instance().get_title(selected_id());
    if (!title) {
        if (header) header->set_select_all_visible(false);
        text_editor_lbl_->setText(obsgs_tr("OBSTitles.LiveTextSelectTitle"));
        text_table_->setEnabled(false);
        if (btn_add_text_row_) btn_add_text_row_->setEnabled(false);
        if (btn_delete_text_row_) btn_delete_text_row_->setEnabled(false);
        if (btn_row_up_) btn_row_up_->setEnabled(false);
        if (btn_row_down_) btn_row_down_->setEnabled(false);
        update_playlist_controls();
        update_persistence_controls();
        update_external_data_controls();
        update_live_text_select_all_state();
        return;
    }

    auto exposed = exposed_text_layers(title);
    normalize_live_text_rows(title, exposed);

    const bool has_exposed = !exposed.empty();
    text_table_->setEnabled(true);
    if (btn_add_text_row_) btn_add_text_row_->setEnabled(has_exposed);
    if (btn_delete_text_row_) btn_delete_text_row_->setEnabled(has_exposed);
    if (btn_row_up_) btn_row_up_->setEnabled(has_exposed);
    if (btn_row_down_) btn_row_down_->setEnabled(has_exposed);
    text_editor_lbl_->setText(obsgs_tr("OBSTitles.LiveTextCues"));
    if (header) header->set_select_all_visible(has_exposed);
    if (!has_exposed) {
        text_table_->setRowCount(1);
        text_table_->setColumnCount(2);
        text_table_->setHorizontalHeaderLabels(QStringList()
                                               << obsgs_tr("OBSTitles.Title")
                                               << QString());
        text_table_->horizontalHeader()->setSectionsMovable(false);
        text_table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
        text_table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
        text_table_->setVerticalHeaderItem(0, new QTableWidgetItem(QStringLiteral("1")));

        auto *title_item = new QTableWidgetItem(QString::fromStdString(title->name));
        title_item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        text_table_->setItem(0, 0, title_item);

        auto *cue = new QPushButton("▶", text_table_);
        cue->setToolTip(obsgs_tr("OBSTitles.PlayCueTooltip"));
        cue->setStyleSheet("QPushButton{background:#2a2a2a;color:#ddd;border:none;border-radius:3px;font-weight:bold;}"
                           "QPushButton:hover{background:#3a3a3a;}");
        connect(cue, &QPushButton::clicked, this, [this]() { cue_live_text_row(0, true); });
        text_table_->setCellWidget(0, 1, cue);
        update_playlist_controls();
        update_persistence_controls();
        update_external_data_controls();
        update_live_text_select_all_state();
        return;
    }

    text_table_->setRowCount((int)title->live_text_rows.size());
    text_table_->setColumnCount((int)exposed.size() + 2);

    QStringList headers;
    headers << "";
    for (const auto &layer : exposed)
        headers << live_text_layer_header(layer);
    headers << "";
    text_table_->setHorizontalHeaderLabels(headers);
    for (int col = 0; col < (int)exposed.size(); ++col) {
        if (auto *item = text_table_->horizontalHeaderItem(col + 1))
            item->setToolTip(live_text_layer_header(exposed[col]));
    }
    text_table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    text_table_->horizontalHeader()->setSectionsMovable(true);
    if (!restore_live_text_header_state()) {
        text_table_->resizeColumnToContents(0);
        text_table_->resizeColumnToContents((int)exposed.size() + 1);
    }

    for (int row = 0; row < (int)title->live_text_rows.size(); ++row) {
        text_table_->setVerticalHeaderItem(row, new QTableWidgetItem(QString::number(row + 1)));
        auto *select_item = new QTableWidgetItem();
        select_item->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        select_item->setCheckState(Qt::Unchecked);
        select_item->setTextAlignment(Qt::AlignCenter);
        text_table_->setItem(row, 0, select_item);
        for (int col = 0; col < (int)exposed.size(); ++col) {
            auto *edit = new QLineEdit(QString::fromStdString(title->live_text_rows[row][col]), text_table_);
            edit->setPlaceholderText(live_text_layer_header(exposed[col]));
            edit->setStyleSheet("QLineEdit{padding:3px;}");
            connect(edit, &QLineEdit::textEdited, this, [this, title, row, col](const QString &text) {
                if (row < 0 || row >= (int)title->live_text_rows.size() ||
                    col < 0 || col >= (int)title->live_text_rows[row].size()) return;
                updating_exposed_text_ = true;
                title->live_text_rows[row][col] = text.toStdString();
                TitleDataStore::instance().save();
                TitleDataStore::instance().touch_runtime_change();
                seen_store_revision_ = TitleDataStore::instance().revision();
                updating_exposed_text_ = false;
            });
            text_table_->setCellWidget(row, col + 1, edit);
        }

        auto *cue = new QPushButton("▶", text_table_);
        cue->setToolTip(obsgs_tr("OBSTitles.PlayCueTooltip"));
        QString cue_style;
        if (row == title->current_cue_row) {
            cue_style = "QPushButton{background:#b02020;color:white;border:none;border-radius:3px;font-weight:bold;}"
                        "QPushButton:hover{background:#d03030;}";
        } else if (row == title->pending_cue_row) {
            cue_style = "QPushButton{background:#1d8f3a;color:white;border:none;border-radius:3px;font-weight:bold;}"
                        "QPushButton:hover{background:#28b84f;}";
        } else {
            cue_style = "QPushButton{background:#2a2a2a;color:#ddd;border:none;border-radius:3px;font-weight:bold;}"
                        "QPushButton:hover{background:#3a3a3a;}";
        }
        cue->setStyleSheet(cue_style);
        connect(cue, &QPushButton::clicked, this, [this, row]() { cue_live_text_row(row, true); });
        text_table_->setCellWidget(row, (int)exposed.size() + 1, cue);
    }
    update_playlist_controls();
    update_persistence_controls();
    update_external_data_controls();
    update_live_text_select_all_state();
}

void TitleDock::on_export_live_text_data()
{
    auto title = TitleDataStore::instance().get_title(selected_id());
    if (!title) return;

    auto exposed = exposed_text_layers(title);
    normalize_live_text_rows(title, exposed);

    QString path = QFileDialog::getSaveFileName(
        this, obsgs_tr("OBSTitles.ExportData"), QString(),
        QStringLiteral("JSON files (*.json);;All files (*.*)"));
    if (path.isEmpty()) return;
    if (!path.endsWith(QStringLiteral(".json"), Qt::CaseInsensitive))
        path += QStringLiteral(".json");

    QJsonArray columns;
    for (const auto &layer : exposed) {
        QJsonObject column;
        column.insert(QStringLiteral("id"), QString::fromStdString(layer->id));
        column.insert(QStringLiteral("name"), live_text_layer_header(layer));
        columns.append(column);
    }

    QJsonObject root;
    root.insert(QStringLiteral("format"), QStringLiteral("OBS Graphics Studio Pro live text cues"));
    root.insert(QStringLiteral("version"), 1);
    root.insert(QStringLiteral("title"), QString::fromStdString(title->name));
    root.insert(QStringLiteral("columns"), columns);
    root.insert(QStringLiteral("rows"), live_text_rows_to_json(title->live_text_rows));

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QMessageBox::warning(this, obsgs_tr("OBSTitles.ExportData"),
                             obsgs_tr("OBSTitles.ExportDataFailed"));
        return;
    }
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    status_lbl_->setText(obsgs_tr("OBSTitles.ExportedStatusFormat").arg(QFileInfo(path).fileName()));
}

void TitleDock::on_import_live_text_data()
{
    auto title = TitleDataStore::instance().get_title(selected_id());
    if (!title) return;

    const QString path = QFileDialog::getOpenFileName(
        this, obsgs_tr("OBSTitles.ImportData"), QString(),
        QStringLiteral("JSON files (*.json);;All files (*.*)"));
    if (path.isEmpty()) return;

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, obsgs_tr("OBSTitles.ImportData"),
                             obsgs_tr("OBSTitles.ImportDataFailed"));
        return;
    }

    QJsonParseError parse_error;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parse_error);
    if (parse_error.error != QJsonParseError::NoError) {
        QMessageBox::warning(this, obsgs_tr("OBSTitles.ImportData"), parse_error.errorString());
        return;
    }

    std::vector<std::vector<std::string>> imported_rows;
    QString error;
    if (!live_text_rows_from_json(doc, imported_rows, &error)) {
        QMessageBox::warning(this, obsgs_tr("OBSTitles.ImportData"), error);
        return;
    }

    auto exposed = exposed_text_layers(title);
    normalize_live_text_rows(title, exposed);
    const int existing_rows = (int)title->live_text_rows.size();
    const int imported_row_count = (int)imported_rows.size();
    if (imported_row_count > existing_rows) {
        const auto answer = QMessageBox::question(
            this, obsgs_tr("OBSTitles.ImportData"),
            obsgs_tr("OBSTitles.ImportDataCropWarning")
                .arg(imported_row_count)
                .arg(existing_rows),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No);
        if (answer != QMessageBox::Yes)
            return;
        imported_rows.resize(existing_rows);
    }

    for (auto &row : imported_rows)
        row.resize(exposed.size());
    title->live_text_rows = std::move(imported_rows);
    normalize_live_text_rows(title, exposed);
    title->current_cue_row = -1;
    title->pending_cue_row = -1;
    ++title->cue_revision;
    TitleDataStore::instance().save();
    TitleDataStore::instance().notify_change();
    populate_exposed_text();
    status_lbl_->setText(obsgs_tr("OBSTitles.ImportedStatusFormat").arg(QFileInfo(path).fileName()));
}

void TitleDock::on_import_append_live_text_data()
{
    auto title = TitleDataStore::instance().get_title(selected_id());
    if (!title) return;

    const QString path = QFileDialog::getOpenFileName(
        this, obsgs_tr("OBSTitles.ImportAppendData"), QString(),
        QStringLiteral("JSON files (*.json);;All files (*.*)"));
    if (path.isEmpty()) return;

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, obsgs_tr("OBSTitles.ImportAppendData"),
                             obsgs_tr("OBSTitles.ImportDataFailed"));
        return;
    }

    QJsonParseError parse_error;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parse_error);
    if (parse_error.error != QJsonParseError::NoError) {
        QMessageBox::warning(this, obsgs_tr("OBSTitles.ImportAppendData"), parse_error.errorString());
        return;
    }

    std::vector<std::vector<std::string>> imported_rows;
    QString error;
    if (!live_text_rows_from_json(doc, imported_rows, &error)) {
        QMessageBox::warning(this, obsgs_tr("OBSTitles.ImportAppendData"), error);
        return;
    }

    auto exposed = exposed_text_layers(title);
    normalize_live_text_rows(title, exposed);
    for (auto &row : imported_rows) {
        row.resize(exposed.size());
        title->live_text_rows.push_back(std::move(row));
    }
    normalize_live_text_rows(title, exposed);
    TitleDataStore::instance().save();
    TitleDataStore::instance().notify_change();
    populate_exposed_text();
    status_lbl_->setText(obsgs_tr("OBSTitles.ImportedStatusFormat").arg(QFileInfo(path).fileName()));
}

void TitleDock::on_toggle_external_data_source()
{
    auto title = TitleDataStore::instance().get_title(selected_id());
    if (!title) return;
    title->external_data_enabled = !title->external_data_enabled;
    TitleDataStore::instance().save();
    TitleDataStore::instance().touch_runtime_change();
    seen_store_revision_ = TitleDataStore::instance().revision();
    update_external_data_controls();
}

void TitleDock::on_show_external_data_settings()
{
    QDialog dialog(this);
    dialog.setWindowTitle(obsgs_tr("OBSTitles.ExternalDataSource"));
    auto *layout = new QVBoxLayout(&dialog);
    auto *header = new QLabel(obsgs_tr("OBSTitles.ExternalDataSource"), &dialog);
    set_bold_label(header);
    layout->addWidget(header);

    auto *tabs = new QTabWidget(&dialog);
    const QStringList names = {
        QStringLiteral("CSV"),
        QStringLiteral("URL"),
        QStringLiteral("RSS Feed"),
        QStringLiteral("Data Sources Settings")
    };
    for (const QString &name : names) {
        auto *tab = new QWidget(tabs);
        auto *tab_layout = new QVBoxLayout(tab);
        auto *placeholder = new QLabel(obsgs_tr("OBSTitles.ExternalDataSettingsPlaceholder"), tab);
        placeholder->setWordWrap(true);
        tab_layout->addWidget(placeholder);
        tab_layout->addStretch();
        tabs->addTab(tab, name);
    }
    layout->addWidget(tabs);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::accept);
    layout->addWidget(buttons);
    dialog.resize(520, 360);
    dialog.exec();
}

void TitleDock::on_refresh_external_data()
{
    auto title = TitleDataStore::instance().get_title(selected_id());
    if (!title) return;
    auto exposed = exposed_text_layers(title);
    normalize_live_text_rows(title, exposed);
    TitleDataStore::instance().save();
    TitleDataStore::instance().touch_runtime_change();
    seen_store_revision_ = TitleDataStore::instance().revision();
    populate_exposed_text();
    status_lbl_->setText(obsgs_tr("OBSTitles.ExternalDataRefreshed"));
}

void TitleDock::on_add_live_text_row()
{
    auto title = TitleDataStore::instance().get_title(selected_id());
    if (!title) return;
    auto exposed = exposed_text_layers(title);
    if (exposed.empty()) return;

    auto selected_rows = selected_live_text_rows();
    std::vector<std::string> row(exposed.size());
    if (selected_rows.size() == 1) {
        const int source_row = selected_rows.front();
        if (source_row >= 0 && source_row < (int)title->live_text_rows.size())
            row = title->live_text_rows[source_row];
    }
    row.resize(exposed.size());

    title->live_text_rows.push_back(std::move(row));
    const int added_row = (int)title->live_text_rows.size() - 1;
    TitleDataStore::instance().save();
    TitleDataStore::instance().notify_change();
    populate_exposed_text();
    apply_live_text_row_selection({added_row}, false);
}

void TitleDock::on_delete_live_text_rows()
{
    auto title = TitleDataStore::instance().get_title(selected_id());
    if (!title || !text_table_) return;

    auto rows = selected_live_text_rows();
    if (rows.empty()) return;

    updating_exposed_text_ = true;
    int next_row = rows.front();
    for (auto it = rows.rbegin(); it != rows.rend(); ++it) {
        const int row = *it;
        if (row < 0 || row >= (int)title->live_text_rows.size())
            continue;
        title->live_text_rows.erase(title->live_text_rows.begin() + row);
        if (title->current_cue_row == row)
            title->current_cue_row = -1;
        else if (title->current_cue_row > row)
            --title->current_cue_row;
        if (title->pending_cue_row == row)
            title->pending_cue_row = -1;
        else if (title->pending_cue_row > row)
            --title->pending_cue_row;
    }

    auto exposed_now = exposed_text_layers(title);
    normalize_live_text_rows(title, exposed_now);
    TitleDataStore::instance().save();
    TitleDataStore::instance().notify_change();
    updating_exposed_text_ = false;
    populate_exposed_text();
    if (!title->live_text_rows.empty())
        text_table_->selectRow(std::min(next_row, (int)title->live_text_rows.size() - 1));
}

void TitleDock::on_move_live_text_row_up()
{
    auto title = TitleDataStore::instance().get_title(selected_id());
    if (!title || !text_table_) return;

    auto rows = selected_live_text_rows();
    if (rows.empty()) return;

    const bool restore_checked = has_checked_live_text_rows();
    const int row_count = (int)title->live_text_rows.size();
    std::vector<bool> selected(row_count, false);
    for (int row : rows) {
        if (row >= 0 && row < row_count)
            selected[row] = true;
    }

    std::vector<int> order(row_count);
    std::iota(order.begin(), order.end(), 0);
    bool moved = false;
    for (int visual = 1; visual < row_count; ++visual) {
        if (selected[order[visual]] && !selected[order[visual - 1]]) {
            std::swap(order[visual], order[visual - 1]);
            moved = true;
        }
    }
    if (!moved) return;

    std::vector<std::vector<std::string>> reordered;
    reordered.reserve(title->live_text_rows.size());
    std::vector<int> new_index(row_count, -1);
    for (int visual = 0; visual < row_count; ++visual) {
        new_index[order[visual]] = visual;
        reordered.push_back(std::move(title->live_text_rows[order[visual]]));
    }
    title->live_text_rows = std::move(reordered);
    if (title->current_cue_row >= 0 && title->current_cue_row < row_count)
        title->current_cue_row = new_index[title->current_cue_row];
    if (title->pending_cue_row >= 0 && title->pending_cue_row < row_count)
        title->pending_cue_row = new_index[title->pending_cue_row];

    std::vector<int> moved_rows;
    for (int row : rows) {
        if (row >= 0 && row < row_count)
            moved_rows.push_back(new_index[row]);
    }
    std::sort(moved_rows.begin(), moved_rows.end());

    TitleDataStore::instance().save();
    TitleDataStore::instance().touch_runtime_change();
    seen_store_revision_ = TitleDataStore::instance().revision();
    populate_exposed_text();
    apply_live_text_row_selection(moved_rows, restore_checked);
}

void TitleDock::on_move_live_text_row_down()
{
    auto title = TitleDataStore::instance().get_title(selected_id());
    if (!title || !text_table_) return;

    auto rows = selected_live_text_rows();
    if (rows.empty()) return;

    const bool restore_checked = has_checked_live_text_rows();
    const int row_count = (int)title->live_text_rows.size();
    std::vector<bool> selected(row_count, false);
    for (int row : rows) {
        if (row >= 0 && row < row_count)
            selected[row] = true;
    }

    std::vector<int> order(row_count);
    std::iota(order.begin(), order.end(), 0);
    bool moved = false;
    for (int visual = row_count - 2; visual >= 0; --visual) {
        if (selected[order[visual]] && !selected[order[visual + 1]]) {
            std::swap(order[visual], order[visual + 1]);
            moved = true;
        }
    }
    if (!moved) return;

    std::vector<std::vector<std::string>> reordered;
    reordered.reserve(title->live_text_rows.size());
    std::vector<int> new_index(row_count, -1);
    for (int visual = 0; visual < row_count; ++visual) {
        new_index[order[visual]] = visual;
        reordered.push_back(std::move(title->live_text_rows[order[visual]]));
    }
    title->live_text_rows = std::move(reordered);
    if (title->current_cue_row >= 0 && title->current_cue_row < row_count)
        title->current_cue_row = new_index[title->current_cue_row];
    if (title->pending_cue_row >= 0 && title->pending_cue_row < row_count)
        title->pending_cue_row = new_index[title->pending_cue_row];

    std::vector<int> moved_rows;
    for (int row : rows) {
        if (row >= 0 && row < row_count)
            moved_rows.push_back(new_index[row]);
    }
    std::sort(moved_rows.begin(), moved_rows.end());

    TitleDataStore::instance().save();
    TitleDataStore::instance().touch_runtime_change();
    seen_store_revision_ = TitleDataStore::instance().revision();
    populate_exposed_text();
    apply_live_text_row_selection(moved_rows, restore_checked);
}


void TitleDock::select_title(const std::string &id)
{
    populate_list();
    for (int i = 0; i < list_->count(); ++i) {
        if (list_->item(i)->data(Qt::UserRole).toString().toStdString() == id) {
            list_->setCurrentRow(i);
            break;
        }
    }
}

std::shared_ptr<Title> TitleDock::create_template_title(const std::string &name,
                                                         int template_id)
{
    auto title = TitleDataStore::instance().create_title(name);
    title->layers.clear();
    title->bg_color = 0x00000000;
    title->duration = 7.0;

    auto add_rect = [&](const std::string &layer_name,
                        double x, double y, float w, float h,
                        uint32_t color, float radius = 0.0f) {
        auto layer = std::make_shared<Layer>();
        layer->id = TitleDataStore::make_uuid();
        layer->name = layer_name;
        layer->type = LayerType::SolidRect;
        layer->pos_x.static_value = x;
        layer->pos_y.static_value = y;
        layer->rect_width = w;
        layer->rect_height = h;
        layer->box_width.static_value = w;
        layer->box_height.static_value = h;
        layer->corner_radius = radius;
        layer->fill_color = color;
        layer->fill_color_a.static_value = (color >> 24) & 0xFF;
        layer->fill_color_r.static_value = (color >> 16) & 0xFF;
        layer->fill_color_g.static_value = (color >> 8) & 0xFF;
        layer->fill_color_b.static_value = color & 0xFF;
        layer->out_time = title->duration;
        title->layers.push_back(layer);
        return layer;
    };

    auto add_text = [&](const std::string &layer_name,
                        const std::string &text,
                        double x, double y, int size,
                        uint32_t color, bool bold = false,
                        int align_h = 1, int align_v = 1) {
        auto layer = std::make_shared<Layer>();
        layer->id = TitleDataStore::make_uuid();
        layer->name = layer_name;
        layer->type = LayerType::Text;
        layer->text_content = text;
        layer->expose_text = true;
        layer->font_family = "Arial";
        layer->font_size = size;
        layer->font_bold = bold;
        layer->text_color = color;
        layer->text_color_a.static_value = (color >> 24) & 0xFF;
        layer->text_color_r.static_value = (color >> 16) & 0xFF;
        layer->text_color_g.static_value = (color >> 8) & 0xFF;
        layer->text_color_b.static_value = color & 0xFF;
        layer->rect_width = 960.0f;
        layer->rect_height = 160.0f;
        layer->box_width.static_value = layer->rect_width;
        layer->box_height.static_value = layer->rect_height;
        layer->pos_x.static_value = x;
        layer->pos_y.static_value = y;
        layer->align_h = align_h;
        layer->align_v = align_v;
        layer->out_time = title->duration;
        title->layers.push_back(layer);
        return layer;
    };

    switch (template_id) {
    case 1: { /* Lower third */
        title->duration = 8.0;
        add_rect(obs_text_std("OBSTitles.LayerLowerThirdBackplate"), 640, 835, 1120, 155, 0xD0161B24, 18.0f);
        add_rect(obs_text_std("OBSTitles.LayerAccentBar"), 120, 835, 18, 155, 0xFF00A3FF, 9.0f);
        add_text(obs_text_std("OBSTitles.LayerName"), name, 670, 800, 58, 0xFFFFFFFF, true, 0, 1);
        add_text(obs_text_std("OBSTitles.LayerSubtitle"), obs_text_std("OBSTitles.TemplateSubtitleRole"), 670, 872, 34, 0xFFE8E8E8, false, 0, 1);
        break;
    }
    case 2: { /* Center title */
        title->duration = 6.0;
        add_rect(obs_text_std("OBSTitles.LayerSoftPanel"), 960, 540, 1280, 270, 0xB0101018, 28.0f);
        add_rect(obs_text_std("OBSTitles.LayerTopAccent"), 960, 395, 520, 10, 0xFF00A3FF, 5.0f);
        add_text(obs_text_std("OBSTitles.LayerMainTitle"), name, 960, 505, 86, 0xFFFFFFFF, true, 1, 1);
        add_text(obs_text_std("OBSTitles.LayerSubtitle"), obs_text_std("OBSTitles.TemplateEditableSubtitle"), 960, 610, 42, 0xFFE0E0E0, false, 1, 1);
        break;
    }
    case 3: { /* Ticker / strap */
        title->duration = 12.0;
        add_rect(obs_text_std("OBSTitles.LayerTickerBackground"), 960, 1010, 1920, 110, 0xE0101010, 0.0f);
        add_rect(obs_text_std("OBSTitles.LayerTickerAccent"), 125, 1010, 250, 110, 0xFF0078D4, 0.0f);
        add_text(obs_text_std("OBSTitles.LayerTickerLabel"), obs_text_std("OBSTitles.TemplateLive"), 125, 1010, 44, 0xFFFFFFFF, true, 1, 1);
        auto ticker = add_text(obs_text_std("OBSTitles.LayerTickerText"), name, 1030, 1010, 44, 0xFFFFFFFF, false, 0, 1);
        ticker->type = LayerType::Ticker;
        ticker->rect_width = 1640.0f;
        ticker->box_width.static_value = ticker->rect_width;
        ticker->ticker_style = 0;
        ticker->ticker_direction = 1;
        ticker->ticker_speed = 140.0;
        break;
    }
    case 4: { /* Clock */
        title->duration = 10.0;
        add_rect(obs_text_std("OBSTitles.ClockBox"), 960, 540, 620, 210, 0xC0101018, 24.0f);
        auto clock = add_text(obs_text_std("OBSTitles.Clock"), name, 960, 530, 92, 0xFFFFFFFF, true, 1, 1);
        clock->type = LayerType::Clock;
        clock->clock_format = "H:i:s";
        add_text(obs_text_std("OBSTitles.LayerSubtitle"), obs_text_std("OBSTitles.TemplateClockSubtitle"), 960, 640, 34, 0xFFE0E0E0, false, 1, 1);
        break;
    }
    case 5: { /* Full screen */
        title->duration = 7.0;
        add_rect(obs_text_std("OBSTitles.LayerSoftPanel"), 960, 540, 1920, 1080, 0xD0101018, 0.0f);
        add_rect(obs_text_std("OBSTitles.LayerTopAccent"), 960, 250, 700, 12, 0xFF00A3FF, 6.0f);
        add_text(obs_text_std("OBSTitles.LayerMainTitle"), name, 960, 480, 96, 0xFFFFFFFF, true, 1, 1);
        add_text(obs_text_std("OBSTitles.LayerSubtitle"), obs_text_std("OBSTitles.TemplateEditableSubtitle"), 960, 610, 48, 0xFFE0E0E0, false, 1, 1);
        break;
    }
    default: {
        add_text(obs_text_std("OBSTitles.TemplateTitleText"), name, 960, 540, 72, 0xFFFFFFFF, true, 1, 1);
        break;
    }
    }

    for (auto &layer : title->layers)
        layer->out_time = title->duration;

    TitleDataStore::instance().notify_change();
    TitleDataStore::instance().save();
    return title;
}

void TitleDock::create_title_from_template(const std::string &default_name,
                                           int template_id)
{
    bool ok = false;
    QString name = QInputDialog::getText(
        this, obsgs_tr("OBSTitles.NewTemplateTitle"), obsgs_tr("OBSTitles.TitleTextPrompt"), QLineEdit::Normal,
        QString::fromStdString(default_name), &ok);
    if (!ok || name.trimmed().isEmpty()) return;

    auto title = create_template_title(name.trimmed().toStdString(), template_id);
    select_title(title->id);
    on_edit();
}

/* ══════════════════════════════════════════════════════════════════
 *  Actions
 * ══════════════════════════════════════════════════════════════════ */
void TitleDock::on_add()
{
    bool ok;
    QString name = QInputDialog::getText(
        this, obsgs_tr("OBSTitles.NewTitle"), obsgs_tr("OBSTitles.TitleNamePrompt"), QLineEdit::Normal, obsgs_tr("OBSTitles.NewTitle"), &ok);
    if (!ok || name.trimmed().isEmpty()) return;

    auto title = TitleDataStore::instance().create_title(name.trimmed().toStdString());
    title->preview_screenshot_png_base64 = title_screenshot_png_base64(title_screenshot_image(*title)).toStdString();
    TitleDataStore::instance().save();
    TitleDataStore::instance().notify_change();
    select_title(title->id);
    on_edit();
}


void TitleDock::on_add_from_templates_library()
{
    const QString root_path = template_library_root_path();
    for (const char *folder : template_library_category_folders)
        QDir(root_path).mkpath(QString::fromUtf8(folder));

    for (const auto &entry : template_library_entries) {
        const QString category_path = QDir(root_path).filePath(QString::fromUtf8(entry.category_folder));
        const QString template_path = QDir(category_path).filePath(QString::fromUtf8(entry.file_name));
        if (QFileInfo::exists(template_path))
            continue;

        auto canned = create_template_title(obs_text_std(entry.default_name_key), entry.id);
        TitleTemplateExportMetadata metadata;
        metadata.title = obs_text_std(entry.name_key);
        metadata.description = obs_text_std(entry.description_key);
        metadata.creator = "OBS Graphics Studio Pro";
        metadata.creation_date = QDateTime::currentDateTimeUtc().toString(Qt::ISODate).toStdString();

        std::string error;
        TitleDataStore::instance().export_title(canned->id, template_path.toStdString(), metadata, &error);
        TitleDataStore::instance().delete_title(canned->id);
        TitleDataStore::instance().save();
    }
    refresh();

    auto *window = new QDialog(this);
    window->setAttribute(Qt::WA_DeleteOnClose);
    window->setWindowTitle(obsgs_tr("OBSTitles.TemplatesLibrary"));
    window->setModal(false);
    window->resize(900, 520);

    auto *layout = new QVBoxLayout(window);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(obs_layout_spacing(window));

    auto *intro = new QLabel(obsgs_tr("OBSTitles.TemplatesLibraryPrompt"), window);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto *splitter = new QSplitter(Qt::Horizontal, window);
    auto *categories = new TemplateCategoryTree(splitter);
    categories->setMinimumWidth(180);
    categories->setAlternatingRowColors(true);
    populate_template_categories(categories, root_path);

    auto *templates = new TemplateListWidget(splitter);
    templates->setViewMode(QListView::IconMode);
    templates->setIconSize(QSize(120, 72));
    templates->setResizeMode(QListView::Adjust);
    templates->setMovement(QListView::Static);
    templates->setSelectionMode(QAbstractItemView::SingleSelection);
    templates->setSpacing(8);

    auto *metadata_panel = new QWidget(splitter);
    auto *metadata_layout = new QVBoxLayout(metadata_panel);
    metadata_layout->setContentsMargins(0, 0, 0, 0);
    auto *metadata_title = new QLabel(obsgs_tr("OBSTitles.TemplateMetadata"), metadata_panel);
    set_bold_label(metadata_title);
    metadata_layout->addWidget(metadata_title);
    auto *preview = new QLabel(metadata_panel);
    preview->setAlignment(Qt::AlignCenter);
    preview->setFrameStyle(QFrame::StyledPanel | QFrame::Sunken);
    preview->setMinimumHeight(140);
    metadata_layout->addWidget(preview);
    auto *details = new QLabel(metadata_panel);
    details->setWordWrap(true);
    details->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    metadata_layout->addWidget(details, 1);

    splitter->addWidget(categories);
    splitter->addWidget(templates);
    splitter->addWidget(metadata_panel);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    splitter->setStretchFactor(2, 0);
    layout->addWidget(splitter, 1);

    auto load_templates_for_category = [templates](const QString &dir_path) {
        templates->clear();
        QDir dir(dir_path);
        const QStringList filters = template_file_filter().split(' ', Qt::SkipEmptyParts);
        for (const QFileInfo &file : dir.entryInfoList(filters, QDir::Files, QDir::Name)) {
            TemplateFileMetadata metadata = read_template_file_metadata(file.absoluteFilePath());
            auto *item = new QListWidgetItem(metadata.screenshot_icon, metadata.title);
            item->setFlags(item->flags() | Qt::ItemIsDragEnabled);
            item->setData(Qt::UserRole, file.absoluteFilePath());
            item->setToolTip(metadata.description);
            templates->addItem(item);
        }
    };
    auto reload_current_category = [categories, load_templates_for_category, templates]() {
        if (categories->currentItem())
            load_templates_for_category(categories->currentItem()->data(0, Qt::UserRole).toString());
        if (templates->count() > 0)
            templates->setCurrentRow(0);
    };
    categories->templates_moved = reload_current_category;

    auto update_metadata = [templates, preview, details]() {
        auto *item = templates->currentItem();
        if (!item) {
            preview->clear();
            details->setText(obsgs_tr("OBSTitles.TemplateNoSelection"));
            return;
        }

        TemplateFileMetadata metadata = read_template_file_metadata(item->data(Qt::UserRole).toString());
        if (!metadata.screenshot_pixmap.isNull()) {
            preview->setPixmap(metadata.screenshot_pixmap.scaled(
                QSize(220, 140), Qt::KeepAspectRatio, Qt::SmoothTransformation));
        } else {
            preview->setText(obsgs_tr("OBSTitles.TemplateScreenshotFailed"));
        }
        details->setText(
            QStringLiteral("<b>%1</b><br><br>%2<br><br><b>%3</b> %4<br><b>%5</b> %6")
                .arg(metadata.title.toHtmlEscaped(),
                     metadata.description.toHtmlEscaped().replace('\n', QStringLiteral("<br>")),
                     obsgs_tr("OBSTitles.TemplateCreatorLabel"), metadata.creator.toHtmlEscaped(),
                     obsgs_tr("OBSTitles.TemplateCreationDateLabel"), metadata.creation_date.toHtmlEscaped()));
    };

    QObject::connect(categories, &QTreeWidget::currentItemChanged, window,
                     [load_templates_for_category, update_metadata](QTreeWidgetItem *current, QTreeWidgetItem *) {
        if (!current) return;
        load_templates_for_category(current->data(0, Qt::UserRole).toString());
        update_metadata();
    });
    QObject::connect(categories, &QTreeWidget::itemDoubleClicked, window,
                     [window, categories, root_path, reload_current_category](QTreeWidgetItem *item, int column) {
        if (!item || column != 0) return;
        if (rename_template_category(window, item)) {
            populate_template_categories(categories, root_path);
            reload_current_category();
        }
    });
    categories->category_delete_requested = [window, categories, root_path, reload_current_category](QTreeWidgetItem *item) {
        if (delete_template_category(window, item, root_path)) {
            populate_template_categories(categories, root_path);
            reload_current_category();
        }
    };
    QObject::connect(templates, &QListWidget::currentItemChanged, window,
                     [update_metadata](QListWidgetItem *, QListWidgetItem *) { update_metadata(); });

    auto import_selected_template = [this, window, templates]() {
        auto *selected = templates->currentItem();
        if (!selected)
            return;

        std::string error;
        auto imported = TitleDataStore::instance().import_title(selected->data(Qt::UserRole).toString().toStdString(), &error);
        if (!imported) {
            QMessageBox::warning(window, obsgs_tr("OBSTitles.ImportTitleTemplate"), QString::fromStdString(error));
            return;
        }
        select_title(imported->id);
        status_lbl_->setText(obsgs_tr("OBSTitles.ImportedStatusFormat").arg(QString::fromStdString(imported->name)));
        window->close();
    };
    QObject::connect(templates, &QListWidget::itemDoubleClicked, window,
                     [import_selected_template](QListWidgetItem *) { import_selected_template(); });

    auto *template_actions = new QHBoxLayout();
    template_actions->setContentsMargins(0, 0, 0, 0);
    auto *edit_template = new QPushButton(obsgs_tr("OBSTitles.Edit"), window);
    auto *delete_template = new QPushButton(obsgs_tr("OBSTitles.Delete"), window);
    template_actions->addWidget(edit_template);
    template_actions->addWidget(delete_template);
    template_actions->addStretch();
    layout->addLayout(template_actions);

    QObject::connect(edit_template, &QPushButton::clicked, window, [window, templates, reload_current_category, update_metadata]() {
        auto *selected = templates->currentItem();
        if (!selected) return;
        if (prompt_edit_template_file_metadata(window, selected->data(Qt::UserRole).toString())) {
            reload_current_category();
            update_metadata();
        }
    });
    QObject::connect(delete_template, &QPushButton::clicked, window, [window, templates, reload_current_category, update_metadata]() {
        auto *selected = templates->currentItem();
        if (!selected) return;
        const QString path = selected->data(Qt::UserRole).toString();
        const auto reply = QMessageBox::question(
            window, obsgs_tr("OBSTitles.DeleteTemplate"),
            obsgs_tr("OBSTitles.DeleteTemplateQuestionFormat").arg(selected->text()),
            QMessageBox::Yes | QMessageBox::No);
        if (reply != QMessageBox::Yes) return;
        if (!QFile::remove(path)) {
            QMessageBox::warning(window, obsgs_tr("OBSTitles.DeleteTemplate"),
                                 obsgs_tr("OBSTitles.DeleteTemplateFailed"));
            return;
        }
        reload_current_category();
        update_metadata();
    });

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel | QDialogButtonBox::Ok, window);
    QObject::connect(buttons, &QDialogButtonBox::accepted, window, import_selected_template);
    QObject::connect(buttons, &QDialogButtonBox::rejected, window, &QDialog::close);
    layout->addWidget(buttons);

    reload_current_category();
    update_metadata();

    window->show();
    window->raise();
    window->activateWindow();
}

void TitleDock::on_duplicate()
{
    auto src = TitleDataStore::instance().get_title(selected_id());
    if (!src) return;

    /* Deep copy by round-tripping through data store */
    auto dup = TitleDataStore::instance().create_title(src->name + obs_text_std("OBSTitles.CopySuffix"));
    dup->description = src->description;
    dup->creator = src->creator;
    dup->creation_date = src->creation_date;
    dup->duration  = src->duration;
    dup->bg_color  = src->bg_color;
    dup->width     = src->width;
    dup->height    = src->height;

    dup->layers.clear();
    for (auto &l : src->layers) {
        auto nl = std::make_shared<Layer>(*l);
        nl->id = TitleDataStore::make_uuid();
        dup->layers.push_back(nl);
    }
    TitleDataStore::instance().notify_change();
    TitleDataStore::instance().save();
    select_title(dup->id);
}

void TitleDock::on_rename()
{
    auto title = TitleDataStore::instance().get_title(selected_id());
    if (!title) return;

    QImage screenshot = title_screenshot_image(*title);
    const QString screenshot_base64 = title_screenshot_png_base64(screenshot);
    if (!screenshot_base64.isEmpty())
        title->preview_screenshot_png_base64 = screenshot_base64.toStdString();

    TitleTemplateExportMetadata metadata;
    if (!prompt_template_metadata(this, *title, screenshot, metadata, nullptr,
                                  obsgs_tr("OBSTitles.EditTemplateMetadata")))
        return;

    title->name = metadata.title;
    title->description = metadata.description;
    title->creator = metadata.creator;
    title->creation_date = metadata.creation_date;
    TitleDataStore::instance().save();
    TitleDataStore::instance().notify_change();
    select_title(title->id);
}

void TitleDock::on_export()
{
    auto title = TitleDataStore::instance().get_title(selected_id());
    if (!title) return;

    QImage screenshot = title_screenshot_image(*title);
    QString screenshot_base64 = title_screenshot_png_base64(screenshot);
    if (screenshot_base64.isEmpty()) {
        QMessageBox::warning(this, obsgs_tr("OBSTitles.ExportTitleTemplate"),
                             obsgs_tr("OBSTitles.TemplateScreenshotFailed"));
        return;
    }

    title->preview_screenshot_png_base64 = screenshot_base64.toStdString();
    TitleDataStore::instance().save();

    TitleTemplateExportMetadata metadata;
    metadata.screenshot_png_base64 = title->preview_screenshot_png_base64;
    bool save_in_template_library = false;
    if (!prompt_template_metadata(this, *title, screenshot, metadata, &save_in_template_library))
        return;

    QString safe_name = QString::fromStdString(metadata.title).trimmed();
    if (safe_name.isEmpty()) safe_name = obsgs_tr("OBSTitles.TemplateFileDialogTitle");
    safe_name.replace(QRegularExpression(QStringLiteral(R"([\\/:*?"<>|])")), QStringLiteral("_"));

    QString path;
    if (save_in_template_library) {
        QString category;
        if (!prompt_template_library_category(this, category))
            return;
        const QString category_path = QDir(template_library_root_path()).filePath(category);
        QDir().mkpath(category_path);
        path = QDir(category_path).filePath(safe_name + QStringLiteral(".ogspt"));
    } else {
        path = QFileDialog::getSaveFileName(
            this, obsgs_tr("OBSTitles.ExportTitleTemplate"),
            QDir(template_library_root_path()).filePath(safe_name + QStringLiteral(".ogspt")),
            obsgs_tr("OBSTitles.TemplateFileFilter"));
        if (path.isEmpty()) return;

        if (QFileInfo(path).suffix().isEmpty())
            path += QStringLiteral(".ogspt");
    }

    std::string error;
    if (!TitleDataStore::instance().export_title(title->id, path.toStdString(), metadata, &error)) {
        QMessageBox::warning(this, obsgs_tr("OBSTitles.ExportTitleTemplate"),
                             QString::fromStdString(error));
        return;
    }

    status_lbl_->setText(obsgs_tr("OBSTitles.ExportedStatusFormat").arg(QFileInfo(path).fileName()));
}

void TitleDock::on_import()
{
    QString path = QFileDialog::getOpenFileName(
        this, obsgs_tr("OBSTitles.ImportTitleTemplate"), QString(),
        obsgs_tr("OBSTitles.TemplateFileFilter"));
    if (path.isEmpty()) return;

    std::string error;
    auto imported = TitleDataStore::instance().import_title(path.toStdString(), &error);
    if (!imported) {
        QMessageBox::warning(this, obsgs_tr("OBSTitles.ImportTitleTemplate"),
                             QString::fromStdString(error));
        return;
    }

    select_title(imported->id);
    status_lbl_->setText(obsgs_tr("OBSTitles.ImportedStatusFormat").arg(QString::fromStdString(imported->name)));
}

void TitleDock::on_delete()
{
    const auto ids = selected_title_ids();
    if (ids.empty()) return;

    QMessageBox::StandardButton reply = QMessageBox::No;
    if (ids.size() == 1) {
        auto title = TitleDataStore::instance().get_title(ids.front());
        if (!title) return;
        reply = confirm_delete_single_title(this, *title);
    } else {
        reply = confirm_delete_multiple_titles(this, (int)ids.size());
    }

    if (reply == QMessageBox::Yes) {
        for (const auto &id : ids)
            TitleDataStore::instance().delete_title(id);
        TitleDataStore::instance().save();
    }
}

void TitleDock::on_edit()
{
    std::string id = selected_id();
    if (id.empty()) return;

    if (!editor_) {
        editor_ = new TitleEditor(
            static_cast<QWidget *>(obs_frontend_get_main_window()));
        editor_->setAttribute(Qt::WA_DeleteOnClose);
        connect(editor_, &QObject::destroyed,
                this, [this]() { editor_ = nullptr; });
        connect(editor_, &TitleEditor::title_saved,
                this, [this](const std::string &) { refresh(); });
    }

    editor_->open_title(id);
    editor_->show();
    editor_->raise();
    editor_->activateWindow();
}

void TitleDock::on_add_to_scene()
{
    std::string id = selected_id();
    if (id.empty()) return;

    auto t = TitleDataStore::instance().get_title(id);
    if (!t) return;

    obs_source_t *scene_source = obs_frontend_get_current_scene();
    if (!scene_source) {
        QMessageBox::warning(this, obsgs_tr("OBSTitles.NoScene"),
                             obsgs_tr("OBSTitles.NoActiveScene"));
        return;
    }

    obs_scene_t *scene = obs_scene_from_source(scene_source);
    if (!scene) {
        obs_source_release(scene_source);
        return;
    }

    /* Create the source */
    obs_data_t *settings = obs_data_create();
    obs_data_set_string(settings, PROP_TITLE_ID, id.c_str());
    obs_data_set_bool  (settings, PROP_LOOP,     true);
    obs_data_set_double(settings, PROP_SPEED,    1.0);

    obs_source_t *source = obs_source_create(
        "obs_graphics_studio_pro_source",
        t->name.c_str(),
        settings,
        nullptr);

    if (source) {
        obs_sceneitem_t *item = obs_scene_add(scene, source);
        if (item) {
            struct vec2 pos = {0.0f, 0.0f};
            obs_sceneitem_set_pos(item, &pos);
            obs_sceneitem_set_visible(item, true);
        }
        obs_source_release(source);
        status_lbl_->setText(obsgs_tr("OBSTitles.AddedToScene"));
    } else {
        QMessageBox::warning(this, obsgs_tr("OBSTitles.AddTitleSource"),
                             obsgs_tr("OBSTitles.CreateSourceFailed"));
    }

    obs_data_release(settings);
    obs_source_release(scene_source);
}
