/*
 * title-dock.h
 *
 * Part 2: OBS Dock – "OBS Titler Pro" panel.
 *
 * Shows a list of all saved titles with:
 *   • Live thumbnail preview
 *   • Add / Delete / Duplicate buttons
 *   • "Edit" button → opens TitleEditor
 *   • "Add to Scene" button → creates/replaces the source in the current scene
 */

#pragma once

#include "title-data.h"
#include <QDockWidget>
#include <QListWidget>
#include <QListWidgetItem>
#include <QTableWidget>
#include <QSplitter>
#include <QPushButton>
#include <QLabel>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QToolBar>
#include <QTimer>

class TitleEditor;

class TitleDock : public QDockWidget {
    Q_OBJECT

public:
    explicit TitleDock(QWidget *parent = nullptr);
    ~TitleDock() override = default;

    /* Called externally to refresh the list (e.g. after editor saves) */
    void refresh();

private slots:
    void on_add();
    void on_add_template_lower_third();
    void on_add_template_center_title();
    void on_add_template_ticker();
    void on_duplicate();
    void on_rename();
    void on_delete();
    void on_export();
    void on_import();
    void on_edit();
    void on_add_to_scene();
    void on_selection_changed();
    void on_add_live_text_row();
    void on_move_live_text_row_up();
    void on_move_live_text_row_down();

private:
    void build_ui();
    void populate_list();
    void populate_exposed_text();
    std::string selected_id() const;
    std::shared_ptr<Title> create_template_title(const std::string &name, int template_id);
    void select_title(const std::string &id);
    void create_title_from_template(const std::string &name, int template_id);

    QWidget      *container_  = nullptr;
    QListWidget  *list_       = nullptr;
    QPushButton  *btn_add_    = nullptr;
    QPushButton  *btn_tpl_    = nullptr;
    QPushButton  *btn_dup_    = nullptr;
    QPushButton  *btn_rename_ = nullptr;
    QPushButton  *btn_del_    = nullptr;
    QPushButton  *btn_export_ = nullptr;
    QPushButton  *btn_import_ = nullptr;
    QPushButton  *btn_edit_   = nullptr;
    QPushButton  *btn_scene_  = nullptr;
    QLabel       *status_lbl_ = nullptr;
    QLabel       *text_editor_lbl_ = nullptr;
    QTableWidget *text_table_ = nullptr;
    QPushButton  *btn_add_text_row_ = nullptr;
    QPushButton  *btn_row_up_ = nullptr;
    QPushButton  *btn_row_down_ = nullptr;
    bool          updating_exposed_text_ = false;
    QTimer       *live_refresh_timer_ = nullptr;
    uint64_t      seen_store_revision_ = 0;

    TitleEditor  *editor_     = nullptr;
};
