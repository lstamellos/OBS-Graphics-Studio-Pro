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

#include <QBuffer>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QInputDialog>
#include <QIODevice>
#include <QItemSelectionModel>
#include <QMenu>
#include <QTextEdit>
#include <QMessageBox>
#include <QVBoxLayout>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QStyle>
#include <QStyleOptionButton>
#include <QToolButton>
#include <QToolBar>
#include <QPushButton>
#include <QListWidget>
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
#include <QSignalBlocker>
#include <QSplitter>
#include <QTableWidgetItem>
#include <QFileDialog>
#include <QFileInfo>
#include <QRegularExpression>
#include <algorithm>
#include <functional>
#include <numeric>
#include <array>

namespace {

static std::vector<std::shared_ptr<Layer>> exposed_text_layers(const std::shared_ptr<Title> &title)
{
    std::vector<std::shared_ptr<Layer>> exposed;
    if (!title) return exposed;
    for (const auto &layer : title->layers) {
        if ((layer->type == LayerType::Text || layer->type == LayerType::Ticker) && layer->expose_text)
            exposed.push_back(layer);
    }
    return exposed;
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

static double title_export_screenshot_time(const Title &title)
{
    const double source_time = title.playback_mode == 2 ? title.pause_time : title.duration * 0.5;
    return std::clamp(source_time, 0.0, std::max(0.0, title.duration));
}

static QImage title_screenshot_image(const Title &title)
{
    return render_title_to_image(title, title_export_screenshot_time(title));
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

static bool prompt_template_export_metadata(QWidget *parent, const Title &title,
                                            const QImage &screenshot,
                                            TitleTemplateExportMetadata &metadata)
{
    QDialog dialog(parent);
    dialog.setWindowTitle(obsgs_tr("OBSTitles.ExportTemplateDetails"));
    dialog.setModal(true);
    dialog.resize(560, 460);

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
    } else {
        preview->setText(obsgs_tr("OBSTitles.TemplateScreenshotFailed"));
    }
    layout->addWidget(preview);

    auto *form = new QFormLayout();
    auto *title_edit = new QLineEdit(QString::fromStdString(title.name), &dialog);
    auto *description_edit = new QTextEdit(&dialog);
    description_edit->setAcceptRichText(false);
    description_edit->setMinimumHeight(96);
    auto *creator_edit = new QLineEdit(&dialog);

    form->addRow(obsgs_tr("OBSTitles.TemplateExportTitleLabel"), title_edit);
    form->addRow(obsgs_tr("OBSTitles.TemplateExportDescriptionLabel"), description_edit);
    form->addRow(obsgs_tr("OBSTitles.TemplateExportCreatorLabel"), creator_edit);
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
    metadata.creation_date = QDateTime::currentDateTimeUtc().toString(Qt::ISODate).toStdString();
    return true;
}

struct TemplateLibraryEntry {
    int id;
    const char *name_key;
    const char *description_key;
    const char *default_name_key;
};

static const std::array<TemplateLibraryEntry, 3> template_library_entries{{
    {1, "OBSTitles.TemplateLowerThird", "OBSTitles.TemplateLowerThirdDescription", "OBSTitles.TemplateSpeakerName"},
    {2, "OBSTitles.TemplateCenteredTitle", "OBSTitles.TemplateCenteredTitleDescription", "OBSTitles.TemplateProgramTitle"},
    {3, "OBSTitles.TemplateTickerStrap", "OBSTitles.TemplateTickerStrapDescription", "OBSTitles.TemplateBreakingNews"},
}};

static const TemplateLibraryEntry *template_library_entry_by_id(int id)
{
    for (const auto &entry : template_library_entries) {
        if (entry.id == id)
            return &entry;
    }
    return nullptr;
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

/* ══════════════════════════════════════════════════════════════════
 *  UI construction
 * ══════════════════════════════════════════════════════════════════ */
void TitleDock::build_ui()
{
    container_ = new QWidget(this);
    auto *root = new QVBoxLayout(container_);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    auto *sections = new QSplitter(Qt::Vertical, container_);
    sections->setChildrenCollapsible(false);
    root->addWidget(sections, 1);

    auto *template_section = new QWidget(sections);
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
    btn_rename_ = make_obs_dock_tool_button(template_toolbar, obsgs_tr("OBSTitles.Rename"), obs_icon("rename.svg"),
                                            obsgs_tr("OBSTitles.RenameTooltip"));
    btn_export_ = make_obs_dock_tool_button(template_toolbar, obsgs_tr("OBSTitles.Export"), obs_icon("export.svg"),
                                            obsgs_tr("OBSTitles.ExportTooltip"));
    btn_edit_ = make_obs_dock_tool_button(template_toolbar, obsgs_tr("OBSTitles.Edit"), obs_icon("edit.svg"),
                                          obsgs_tr("OBSTitles.EditTooltip"));
    btn_scene_ = make_obs_dock_tool_button(template_toolbar, obsgs_tr("OBSTitles.AddToScene"), obs_icon("add-to-scene.svg"),
                                           obsgs_tr("OBSTitles.AddToSceneTooltip"));

    template_toolbar->addWidget(btn_add_);
    template_toolbar->addSeparator();
    template_toolbar->addWidget(btn_dup_);
    template_toolbar->addWidget(btn_del_);
    template_toolbar->addWidget(toolbar_spacer(template_toolbar));
    template_toolbar->addWidget(btn_rename_);
    template_toolbar->addWidget(btn_export_);
    template_toolbar->addWidget(btn_edit_);
    template_toolbar->addWidget(btn_scene_);

    /* ── template/title section ── */
    auto *template_header = new QHBoxLayout();
    template_header->setContentsMargins(0, 0, 0, 0);
    template_header->setSpacing(0);

    auto *template_lbl = new QLabel(obsgs_tr("OBSTitles.TitleTemplates"), template_section);
    set_bold_label(template_lbl);
    template_header->addWidget(template_lbl);
    template_header->addStretch();
    template_header->addWidget(template_toolbar);
    template_layout->addLayout(template_header);

    list_ = new QListWidget(template_section);
    list_->setAlternatingRowColors(true);
    list_->setSelectionMode(QAbstractItemView::SingleSelection);
    list_->setMinimumHeight(120);
    template_layout->addWidget(list_, 1);

    auto *live_section = new QWidget(sections);
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
    live_toolbar->addWidget(btn_add_text_row_);
    live_toolbar->addWidget(btn_delete_text_row_);
    live_toolbar->addWidget(btn_row_up_);
    live_toolbar->addWidget(btn_row_down_);

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

    sections->addWidget(template_section);
    sections->addWidget(live_section);
    sections->setStretchFactor(0, 2);
    sections->setStretchFactor(1, 1);

    /* ── status ── */
    status_lbl_ = new QLabel(obsgs_tr("OBSTitles.NoTitleSelected"), container_);
    status_lbl_->setAlignment(Qt::AlignCenter);
    QFont sf = status_lbl_->font();
    sf.setPointSize(std::max(1, sf.pointSize() - 1));
    status_lbl_->setFont(sf);
    template_layout->addWidget(status_lbl_);

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
    connect(btn_add_text_row_, &QToolButton::clicked, this, &TitleDock::on_add_live_text_row);
    connect(btn_delete_text_row_, &QToolButton::clicked, this, &TitleDock::on_delete_live_text_rows);
    connect(btn_row_up_, &QToolButton::clicked, this, &TitleDock::on_move_live_text_row_up);
    connect(btn_row_down_, &QToolButton::clicked, this, &TitleDock::on_move_live_text_row_down);
    connect(text_table_, &QTableWidget::itemChanged, this, [this](QTableWidgetItem *item) {
        if (item && item->column() == 0)
            update_live_text_select_all_state();
    });
    connect(text_table_->horizontalHeader(), &QHeaderView::sectionMoved,
            this, [this](int, int, int) { save_live_text_header_state(); });
    connect(text_table_->horizontalHeader(), &QHeaderView::sectionResized,
            this, [this](int, int, int) { save_live_text_header_state(); });
    connect(list_, &QListWidget::itemSelectionChanged,
            this, &TitleDock::on_selection_changed);
    connect(list_, &QListWidget::itemDoubleClicked,
            this, [this](QListWidgetItem *) { on_edit(); });

    on_selection_changed();
}

/* ══════════════════════════════════════════════════════════════════
 *  List population
 * ══════════════════════════════════════════════════════════════════ */
void TitleDock::populate_list()
{
    QString prev_id = QString::fromStdString(selected_id());
    list_->blockSignals(true);
    list_->clear();

    for (auto &t : TitleDataStore::instance().titles()) {
        auto *item = new QListWidgetItem(QString::fromStdString(t->name));
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

void TitleDock::on_selection_changed()
{
    bool has = !selected_id().empty();
    btn_dup_->setEnabled(has);
    btn_rename_->setEnabled(has);
    btn_del_->setEnabled(has);
    btn_export_->setEnabled(has);
    btn_edit_->setEnabled(has);
    btn_scene_->setEnabled(has);

    if (has) {
        auto t = TitleDataStore::instance().get_title(selected_id());
        if (t)
            status_lbl_->setText(
                obsgs_tr("OBSTitles.StatusLayerCountFormat")
                    .arg(t->layers.size())
                    .arg(t->duration, 0, 'f', 1));
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
    live_text_header_states_[text_table_->columnCount()] = text_table_->horizontalHeader()->saveState();
}

bool TitleDock::restore_live_text_header_state()
{
    if (!text_table_ || text_table_->columnCount() <= 0) return false;
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
        connect(cue, &QPushButton::clicked, this, [this, title]() {
            updating_exposed_text_ = true;
            title->current_cue_row = -1;
            title->pending_cue_row = -1;
            ++title->cue_revision;
            TitleDataStore::instance().save();
            TitleDataStore::instance().notify_change();
            updating_exposed_text_ = false;
            populate_exposed_text();
        });
        text_table_->setCellWidget(0, 1, cue);
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
        connect(cue, &QPushButton::clicked, this, [this, title, row]() {
            auto exposed_now = exposed_text_layers(title);
            normalize_live_text_rows(title, exposed_now);
            if (row < 0 || row >= (int)title->live_text_rows.size()) return;
            updating_exposed_text_ = true;
            const bool needs_outro_before_cue =
                (title->playback_mode == 1 || title->playback_mode == 2) &&
                title->current_cue_row >= 0 && title->current_cue_row != row;
            if (needs_outro_before_cue) {
                title->pending_cue_row = row;
            } else {
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
        });
        text_table_->setCellWidget(row, (int)exposed.size() + 1, cue);
    }
    update_live_text_select_all_state();
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
    TitleDataStore::instance().notify_change();
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
    TitleDataStore::instance().notify_change();
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
    TitleDataStore::instance().save();
    TitleDataStore::instance().notify_change();
    select_title(title->id);
    on_edit();
}


void TitleDock::on_add_from_templates_library()
{
    auto *window = new QDialog(this);
    window->setAttribute(Qt::WA_DeleteOnClose);
    window->setWindowTitle(obsgs_tr("OBSTitles.TemplatesLibrary"));
    window->setModal(false);
    window->resize(520, 360);

    auto *layout = new QVBoxLayout(window);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(obs_layout_spacing(window));

    auto *intro = new QLabel(obsgs_tr("OBSTitles.TemplatesLibraryPrompt"), window);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto *templates = new QListWidget(window);
    templates->setSelectionMode(QAbstractItemView::SingleSelection);
    for (const auto &entry : template_library_entries) {
        auto *item = new QListWidgetItem(obsgs_tr(entry.name_key));
        item->setData(Qt::UserRole, entry.id);
        item->setToolTip(obsgs_tr(entry.description_key));
        templates->addItem(item);
    }
    layout->addWidget(templates, 1);

    auto *description = new QLabel(window);
    description->setWordWrap(true);
    description->setFrameStyle(QFrame::StyledPanel | QFrame::Sunken);
    description->setMinimumHeight(64);
    layout->addWidget(description);

    auto update_description = [templates, description]() {
        auto *item = templates->currentItem();
        if (!item) {
            description->clear();
            return;
        }
        const auto *entry = template_library_entry_by_id(item->data(Qt::UserRole).toInt());
        description->setText(entry ? obsgs_tr(entry->description_key) : QString());
    };
    QObject::connect(templates, &QListWidget::currentItemChanged, window,
                     [update_description](QListWidgetItem *, QListWidgetItem *) { update_description(); });

    auto add_selected_template = [this, window, templates]() {
        auto *selected = templates->currentItem();
        if (!selected)
            return;

        const auto *entry = template_library_entry_by_id(selected->data(Qt::UserRole).toInt());
        if (!entry)
            return;

        window->close();
        create_title_from_template(obs_text_std(entry->default_name_key), entry->id);
    };
    QObject::connect(templates, &QListWidget::itemDoubleClicked, window,
                     [add_selected_template](QListWidgetItem *) { add_selected_template(); });

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel | QDialogButtonBox::Ok, window);
    QObject::connect(buttons, &QDialogButtonBox::accepted, window, add_selected_template);
    QObject::connect(buttons, &QDialogButtonBox::rejected, window, &QDialog::close);
    layout->addWidget(buttons);

    if (templates->count() > 0)
        templates->setCurrentRow(0);
    update_description();

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

    bool ok = false;
    QString name = QInputDialog::getText(
        this, obsgs_tr("OBSTitles.RenameTitleTemplate"), obsgs_tr("OBSTitles.TemplateNamePrompt"), QLineEdit::Normal,
        QString::fromStdString(title->name), &ok);
    name = name.trimmed();
    if (!ok || name.isEmpty()) return;

    TitleDataStore::instance().rename_title(title->id, name.toStdString());
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

    TitleTemplateExportMetadata metadata;
    metadata.screenshot_png_base64 = screenshot_base64.toStdString();
    if (!prompt_template_export_metadata(this, *title, screenshot, metadata))
        return;

    QString safe_name = QString::fromStdString(metadata.title).trimmed();
    if (safe_name.isEmpty()) safe_name = obsgs_tr("OBSTitles.TemplateFileDialogTitle");
    safe_name.replace(QRegularExpression(QStringLiteral(R"([\\/:*?"<>|])")), QStringLiteral("_"));

    QString path = QFileDialog::getSaveFileName(
        this, obsgs_tr("OBSTitles.ExportTitleTemplate"), safe_name + QStringLiteral(".ogspt"),
        obsgs_tr("OBSTitles.TemplateFileFilter"));
    if (path.isEmpty()) return;

    if (QFileInfo(path).suffix().isEmpty())
        path += QStringLiteral(".ogspt");

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
    std::string id = selected_id();
    if (id.empty()) return;

    auto t = TitleDataStore::instance().get_title(id);
    if (!t) return;

    auto reply = QMessageBox::question(
        this, obsgs_tr("OBSTitles.DeleteTitle"),
        obsgs_tr("OBSTitles.DeleteTitleQuestionFormat").arg(QString::fromStdString(t->name)),
        QMessageBox::Yes | QMessageBox::No);

    if (reply == QMessageBox::Yes) {
        TitleDataStore::instance().delete_title(id);
        TitleDataStore::instance().save();
        TitleDataStore::instance().notify_change();
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
