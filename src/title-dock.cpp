/*
 * title-dock.cpp
 */

#include "title-dock.h"
#include "title-editor.h"
#include "title-data.h"
#include "title-source.h"

#include <obs-module.h>
#include <obs-frontend-api.h>

#include <QInputDialog>
#include <QMenu>
#include <QMessageBox>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QFont>
#include <QSizePolicy>
#include <QString>
#include <QHeaderView>
#include <QLineEdit>
#include <QSignalBlocker>
#include <QSplitter>
#include <QFileDialog>
#include <QFileInfo>
#include <QRegularExpression>

namespace {

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

static QString live_text_layer_header(const std::shared_ptr<Layer> &layer)
{
    if (!layer) return QStringLiteral("Text");
    QString name = QString::fromStdString(layer->name).trimmed();
    if (!name.isEmpty()) return name;
    name = QString::fromStdString(layer->text_content).trimmed();
    return name.isEmpty() ? QStringLiteral("Text") : name;
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

static void move_live_row_marker(int &marker, int from, int to)
{
    if (marker == from) marker = to;
    else if (marker == to) marker = from;
}

} // namespace

/* ══════════════════════════════════════════════════════════════════
 *  Constructor
 * ══════════════════════════════════════════════════════════════════ */
TitleDock::TitleDock(QWidget *parent)
    : QDockWidget("OBS Titler Pro", parent)
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
    root->setContentsMargins(4, 4, 4, 4);
    root->setSpacing(4);

    /* ── header toolbar ── */
    auto *toolbar = new QHBoxLayout();
    toolbar->setSpacing(2);

    btn_add_  = new QPushButton("+",        container_);
    btn_tpl_  = new QPushButton("Templates", container_);
    btn_dup_  = new QPushButton("⧉",        container_);
    btn_rename_ = new QPushButton("Rename", container_);
    btn_del_  = new QPushButton("✕",        container_);
    btn_export_ = new QPushButton("Export", container_);
    btn_import_ = new QPushButton("Import", container_);
    btn_edit_ = new QPushButton("Edit …",   container_);
    btn_scene_= new QPushButton("▶ Scene",  container_);

    btn_add_->setToolTip("New blank title");
    btn_tpl_->setToolTip("Create a title from a Titler-style template");
    btn_dup_->setToolTip("Duplicate");
    btn_rename_->setToolTip("Rename selected title template");
    btn_del_->setToolTip("Delete");
    btn_export_->setToolTip("Export selected title template to a file");
    btn_import_->setToolTip("Import a title template file");
    btn_edit_->setToolTip("Open title editor");
    btn_scene_->setToolTip("Add selected title to current scene");

    for (auto *b : {btn_add_, btn_dup_, btn_del_})
        b->setFixedWidth(28);
    btn_tpl_->setFixedHeight(24);
    btn_rename_->setFixedHeight(24);
    btn_export_->setFixedHeight(24);
    btn_import_->setFixedHeight(24);
    btn_edit_->setFixedHeight(24);
    btn_scene_->setFixedHeight(24);

    toolbar->addWidget(btn_add_);
    toolbar->addWidget(btn_tpl_);
    toolbar->addWidget(btn_import_);
    toolbar->addWidget(btn_dup_);
    toolbar->addWidget(btn_del_);
    toolbar->addStretch();
    toolbar->addWidget(btn_rename_);
    toolbar->addWidget(btn_export_);
    toolbar->addWidget(btn_edit_);
    toolbar->addWidget(btn_scene_);

    auto *sections = new QSplitter(Qt::Vertical, container_);
    sections->setChildrenCollapsible(false);
    root->addWidget(sections, 1);

    auto *template_section = new QWidget(sections);
    auto *template_layout = new QVBoxLayout(template_section);
    template_layout->setContentsMargins(0, 0, 0, 0);
    template_layout->setSpacing(4);
    template_layout->addLayout(toolbar);

    /* ── template/title section ── */
    auto *template_lbl = new QLabel("Title templates", template_section);
    template_lbl->setStyleSheet("font-weight:bold;color:#ddd;");
    template_layout->addWidget(template_lbl);

    list_ = new QListWidget(template_section);
    list_->setAlternatingRowColors(true);
    list_->setSelectionMode(QAbstractItemView::SingleSelection);
    list_->setMinimumHeight(120);
    template_layout->addWidget(list_, 1);

    auto *live_section = new QWidget(sections);
    auto *live_layout = new QVBoxLayout(live_section);
    live_layout->setContentsMargins(0, 0, 0, 0);
    live_layout->setSpacing(4);

    auto *live_header = new QHBoxLayout();
    /* ── exposed text section ── */
    text_editor_lbl_ = new QLabel("Live text", live_section);
    text_editor_lbl_->setStyleSheet("font-weight:bold;color:#ddd;margin-top:4px;");
    btn_add_text_row_ = new QPushButton("+ Row", live_section);
    btn_add_text_row_->setToolTip("Add another live text cue row");
    btn_add_text_row_->setFixedHeight(22);
    btn_row_up_ = new QPushButton("▲", live_section);
    btn_row_up_->setToolTip("Move selected cue row up");
    btn_row_up_->setFixedSize(24, 22);
    btn_row_down_ = new QPushButton("▼", live_section);
    btn_row_down_->setToolTip("Move selected cue row down");
    btn_row_down_->setFixedSize(24, 22);
    live_header->addWidget(text_editor_lbl_);
    live_header->addStretch();
    live_header->addWidget(btn_row_up_);
    live_header->addWidget(btn_row_down_);
    live_header->addWidget(btn_add_text_row_);
    live_layout->addLayout(live_header);

    text_table_ = new QTableWidget(live_section);
    text_table_->setMinimumHeight(96);
    text_table_->setAlternatingRowColors(false);
    text_table_->verticalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    text_table_->verticalHeader()->setDefaultSectionSize(30);
    text_table_->horizontalHeader()->setStretchLastSection(false);
    text_table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    text_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    text_table_->setSelectionMode(QAbstractItemView::SingleSelection);
    text_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    live_layout->addWidget(text_table_, 1);

    sections->addWidget(template_section);
    sections->addWidget(live_section);
    sections->setStretchFactor(0, 2);
    sections->setStretchFactor(1, 1);

    /* ── status ── */
    status_lbl_ = new QLabel("No title selected", container_);
    status_lbl_->setAlignment(Qt::AlignCenter);
    QFont sf = status_lbl_->font();
    sf.setPointSize(sf.pointSize() - 1);
    status_lbl_->setFont(sf);
    template_layout->addWidget(status_lbl_);

    setWidget(container_);

    /* ── connections ── */
    auto *template_menu = new QMenu(btn_tpl_);
    template_menu->addAction("Lower Third", this, &TitleDock::on_add_template_lower_third);
    template_menu->addAction("Centered Title", this, &TitleDock::on_add_template_center_title);
    template_menu->addAction("Ticker / Strap", this, &TitleDock::on_add_template_ticker);
    btn_tpl_->setMenu(template_menu);

    connect(btn_add_,   &QPushButton::clicked, this, &TitleDock::on_add);
    connect(btn_dup_,   &QPushButton::clicked, this, &TitleDock::on_duplicate);
    connect(btn_rename_, &QPushButton::clicked, this, &TitleDock::on_rename);
    connect(btn_del_,   &QPushButton::clicked, this, &TitleDock::on_delete);
    connect(btn_export_, &QPushButton::clicked, this, &TitleDock::on_export);
    connect(btn_import_, &QPushButton::clicked, this, &TitleDock::on_import);
    connect(btn_edit_,  &QPushButton::clicked, this, &TitleDock::on_edit);
    connect(btn_scene_, &QPushButton::clicked, this, &TitleDock::on_add_to_scene);
    connect(btn_add_text_row_, &QPushButton::clicked, this, &TitleDock::on_add_live_text_row);
    connect(btn_row_up_, &QPushButton::clicked, this, &TitleDock::on_move_live_text_row_up);
    connect(btn_row_down_, &QPushButton::clicked, this, &TitleDock::on_move_live_text_row_down);
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
            QString("%1 layer(s)  |  %.1fs").arg(t->layers.size()).arg(t->duration));
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
                QString("%1 layers  ·  %2s")
                    .arg(t->layers.size())
                    .arg(t->duration, 0, 'f', 1));
    } else {
        status_lbl_->setText(list_->count() == 0
            ? "Click + or Templates to create a title"
            : "No title selected");
    }
    populate_exposed_text();
}

void TitleDock::populate_exposed_text()
{
    if (!text_table_) return;
    QSignalBlocker block(text_table_);
    text_table_->clear();
    text_table_->setRowCount(0);
    text_table_->setColumnCount(0);

    auto title = TitleDataStore::instance().get_title(selected_id());
    if (!title) {
        text_editor_lbl_->setText("Live text — select a title");
        text_table_->setEnabled(false);
        if (btn_add_text_row_) btn_add_text_row_->setEnabled(false);
        if (btn_row_up_) btn_row_up_->setEnabled(false);
        if (btn_row_down_) btn_row_down_->setEnabled(false);
        return;
    }

    auto exposed = exposed_text_layers(title);
    normalize_live_text_rows(title, exposed);

    const bool has_exposed = !exposed.empty();
    text_table_->setEnabled(has_exposed);
    if (btn_add_text_row_) btn_add_text_row_->setEnabled(has_exposed);
    if (btn_row_up_) btn_row_up_->setEnabled(has_exposed);
    if (btn_row_down_) btn_row_down_->setEnabled(has_exposed);
    text_editor_lbl_->setText(has_exposed
        ? "Live text cues"
        : "Live text — expose text layers in the editor");
    if (!has_exposed) return;

    text_table_->setRowCount((int)title->live_text_rows.size());
    text_table_->setColumnCount((int)exposed.size() + 2);

    QStringList headers;
    for (const auto &layer : exposed)
        headers << live_text_layer_header(layer);
    headers << "" << "";
    text_table_->setHorizontalHeaderLabels(headers);
    for (int col = 0; col < (int)exposed.size(); ++col) {
        if (auto *item = text_table_->horizontalHeaderItem(col))
            item->setToolTip(live_text_layer_header(exposed[col]));
    }
    for (int col = 0; col < (int)exposed.size(); ++col)
        text_table_->horizontalHeader()->setSectionResizeMode(col, QHeaderView::Stretch);
    text_table_->horizontalHeader()->setSectionResizeMode((int)exposed.size(), QHeaderView::ResizeToContents);
    text_table_->horizontalHeader()->setSectionResizeMode((int)exposed.size() + 1, QHeaderView::ResizeToContents);

    for (int row = 0; row < (int)title->live_text_rows.size(); ++row) {
        text_table_->setVerticalHeaderItem(row, new QTableWidgetItem(QString::number(row + 1)));
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
            text_table_->setCellWidget(row, col, edit);
        }

        auto *cue = new QPushButton("▶", text_table_);
        cue->setToolTip("Play this row and run the intro/loop/outro animation");
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
        text_table_->setCellWidget(row, (int)exposed.size(), cue);

        auto *del = new QPushButton("✕", text_table_);
        del->setToolTip("Delete this live text row");
        connect(del, &QPushButton::clicked, this, [this, title, row]() {
            if (row < 0 || row >= (int)title->live_text_rows.size()) return;
            updating_exposed_text_ = true;
            title->live_text_rows.erase(title->live_text_rows.begin() + row);
            if (title->current_cue_row == row)
                title->current_cue_row = -1;
            else if (title->current_cue_row > row)
                --title->current_cue_row;
            if (title->pending_cue_row == row)
                title->pending_cue_row = -1;
            else if (title->pending_cue_row > row)
                --title->pending_cue_row;
            auto exposed_now = exposed_text_layers(title);
            normalize_live_text_rows(title, exposed_now);
            TitleDataStore::instance().save();
            TitleDataStore::instance().notify_change();
            updating_exposed_text_ = false;
            populate_exposed_text();
        });
        text_table_->setCellWidget(row, (int)exposed.size() + 1, del);
    }
}

void TitleDock::on_add_live_text_row()
{
    auto title = TitleDataStore::instance().get_title(selected_id());
    if (!title) return;
    auto exposed = exposed_text_layers(title);
    if (exposed.empty()) return;

    std::vector<std::string> row;
    for (const auto &layer : exposed)
        row.push_back(layer->text_content);
    title->live_text_rows.push_back(std::move(row));
    TitleDataStore::instance().save();
    TitleDataStore::instance().notify_change();
    populate_exposed_text();
    text_table_->selectRow((int)title->live_text_rows.size() - 1);
}

void TitleDock::on_move_live_text_row_up()
{
    auto title = TitleDataStore::instance().get_title(selected_id());
    if (!title || !text_table_) return;
    int row = text_table_->currentRow();
    if (row <= 0 || row >= (int)title->live_text_rows.size()) return;
    std::swap(title->live_text_rows[row], title->live_text_rows[row - 1]);
    move_live_row_marker(title->current_cue_row, row, row - 1);
    move_live_row_marker(title->pending_cue_row, row, row - 1);
    TitleDataStore::instance().save();
    TitleDataStore::instance().notify_change();
    populate_exposed_text();
    text_table_->selectRow(row - 1);
}

void TitleDock::on_move_live_text_row_down()
{
    auto title = TitleDataStore::instance().get_title(selected_id());
    if (!title || !text_table_) return;
    int row = text_table_->currentRow();
    if (row < 0 || row + 1 >= (int)title->live_text_rows.size()) return;
    std::swap(title->live_text_rows[row], title->live_text_rows[row + 1]);
    move_live_row_marker(title->current_cue_row, row, row + 1);
    move_live_row_marker(title->pending_cue_row, row, row + 1);
    TitleDataStore::instance().save();
    TitleDataStore::instance().notify_change();
    populate_exposed_text();
    text_table_->selectRow(row + 1);
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
    case 1: /* Lower third */
        title->duration = 8.0;
        add_rect("Lower Third Backplate", 640, 835, 1120, 155, 0xD0161B24, 18.0f);
        add_rect("Accent Bar", 120, 835, 18, 155, 0xFF00A3FF, 9.0f);
        add_text("Name", name, 670, 800, 58, 0xFFFFFFFF, true, 0, 1);
        add_text("Subtitle", "Subtitle / role", 670, 872, 34, 0xFFE8E8E8, false, 0, 1);
        break;
    case 2: /* Center title */
        title->duration = 6.0;
        add_rect("Soft Panel", 960, 540, 1280, 270, 0xB0101018, 28.0f);
        add_rect("Top Accent", 960, 395, 520, 10, 0xFF00A3FF, 5.0f);
        add_text("Main Title", name, 960, 505, 86, 0xFFFFFFFF, true, 1, 1);
        add_text("Subtitle", "Editable subtitle", 960, 610, 42, 0xFFE0E0E0, false, 1, 1);
        break;
    case 3: /* Ticker / strap */
        title->duration = 12.0;
        add_rect("Ticker Background", 960, 1010, 1920, 110, 0xE0101010, 0.0f);
        add_rect("Ticker Accent", 125, 1010, 250, 110, 0xFF0078D4, 0.0f);
        add_text("Ticker Label", "LIVE", 125, 1010, 44, 0xFFFFFFFF, true, 1, 1);
        add_text("Ticker Text", name, 1030, 1010, 44, 0xFFFFFFFF, false, 0, 1);
        break;
    default:
        add_text("Title Text", name, 960, 540, 72, 0xFFFFFFFF, true, 1, 1);
        break;
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
        this, "New Template Title", "Title text:", QLineEdit::Normal,
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
        this, "New Title", "Title name:", QLineEdit::Normal, "New Title", &ok);
    if (!ok || name.trimmed().isEmpty()) return;

    auto title = TitleDataStore::instance().create_title(name.trimmed().toStdString());
    TitleDataStore::instance().save();
    select_title(title->id);
    on_edit();
}

void TitleDock::on_add_template_lower_third()
{
    create_title_from_template("Speaker Name", 1);
}

void TitleDock::on_add_template_center_title()
{
    create_title_from_template("Program Title", 2);
}

void TitleDock::on_add_template_ticker()
{
    create_title_from_template("Breaking news headline goes here", 3);
}

void TitleDock::on_duplicate()
{
    auto src = TitleDataStore::instance().get_title(selected_id());
    if (!src) return;

    /* Deep copy by round-tripping through data store */
    auto dup = TitleDataStore::instance().create_title(src->name + " (copy)");
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
        this, "Rename Title Template", "Template name:", QLineEdit::Normal,
        QString::fromStdString(title->name), &ok);
    name = name.trimmed();
    if (!ok || name.isEmpty()) return;

    TitleDataStore::instance().rename_title(title->id, name.toStdString());
    TitleDataStore::instance().save();
    select_title(title->id);
}

void TitleDock::on_export()
{
    auto title = TitleDataStore::instance().get_title(selected_id());
    if (!title) return;

    QString safe_name = QString::fromStdString(title->name).trimmed();
    if (safe_name.isEmpty()) safe_name = QStringLiteral("OBS Titler Pro Template");
    safe_name.replace(QRegularExpression(QStringLiteral(R"([\\/:*?"<>|])")), QStringLiteral("_"));

    QString path = QFileDialog::getSaveFileName(
        this, "Export Title Template", safe_name + QStringLiteral(".otpt"),
        "OBS Titler Pro Templates (*.otpt *.json);;JSON Files (*.json);;All Files (*)");
    if (path.isEmpty()) return;

    if (QFileInfo(path).suffix().isEmpty())
        path += QStringLiteral(".otpt");

    std::string error;
    if (!TitleDataStore::instance().export_title(title->id, path.toStdString(), &error)) {
        QMessageBox::warning(this, "Export Title Template",
                             QString::fromStdString(error));
        return;
    }

    status_lbl_->setText(QString("Exported %1").arg(QFileInfo(path).fileName()));
}

void TitleDock::on_import()
{
    QString path = QFileDialog::getOpenFileName(
        this, "Import Title Template", QString(),
        "OBS Titler Pro Templates (*.otpt *.json);;JSON Files (*.json);;All Files (*)");
    if (path.isEmpty()) return;

    std::string error;
    auto imported = TitleDataStore::instance().import_title(path.toStdString(), &error);
    if (!imported) {
        QMessageBox::warning(this, "Import Title Template",
                             QString::fromStdString(error));
        return;
    }

    select_title(imported->id);
    status_lbl_->setText(QString("Imported %1").arg(QString::fromStdString(imported->name)));
}

void TitleDock::on_delete()
{
    std::string id = selected_id();
    if (id.empty()) return;

    auto t = TitleDataStore::instance().get_title(id);
    if (!t) return;

    auto reply = QMessageBox::question(
        this, "Delete Title",
        QString("Delete \"%1\"?").arg(QString::fromStdString(t->name)),
        QMessageBox::Yes | QMessageBox::No);

    if (reply == QMessageBox::Yes) {
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
        QMessageBox::warning(this, "No Scene",
                             "There is no active scene to add the title to.");
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
        "obs_titles_source",
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
        status_lbl_->setText("Added to scene");
    } else {
        QMessageBox::warning(this, "Add Title Source",
                             "OBS could not create the Title source.");
    }

    obs_data_release(settings);
    obs_source_release(scene_source);
}
