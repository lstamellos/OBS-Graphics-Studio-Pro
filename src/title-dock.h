/*
 * title-dock.h
 *
 * Part 2: OBS Dock – "OBS Graphics Studio Pro" panel.
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
#include <QToolButton>
#include <QLabel>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QToolBar>
#include <QTimer>
#include <QByteArray>
#include <map>

class TitleEditor;

class TitleDock : public QDockWidget {
    Q_OBJECT

public:
    explicit TitleDock(QWidget *parent = nullptr);
    ~TitleDock() override = default;

    /* Called externally to refresh the list (e.g. after editor saves) */
    void refresh();
    void update_scene_collection_title();

private slots:
    void on_add();
    void on_add_from_templates_library();
    void on_duplicate();
    void on_rename();
    void on_delete();
    void on_export();
    void on_import();
    void on_edit();
    void on_add_to_scene();
    void on_selection_changed();
    void on_add_live_text_row();
    void on_delete_live_text_rows();
    void on_move_live_text_row_up();
    void on_move_live_text_row_down();

private:
    void build_ui();
    void populate_list();
    void populate_exposed_text();
    void set_all_live_text_rows_checked(bool checked);
    void update_live_text_select_all_state();
    void save_live_text_header_state();
    bool restore_live_text_header_state();
    bool has_checked_live_text_rows() const;
    void apply_live_text_row_selection(const std::vector<int> &rows, bool checked);
    std::string selected_id() const;
    std::shared_ptr<Title> create_template_title(const std::string &name, int template_id);
    void select_title(const std::string &id);
    void create_title_from_template(const std::string &name, int template_id);
    std::vector<int> selected_live_text_rows() const;

    QWidget      *container_  = nullptr;
    QListWidget  *list_       = nullptr;
    QToolButton *btn_add_    = nullptr;
    QToolButton *btn_dup_    = nullptr;
    QToolButton *btn_rename_ = nullptr;
    QToolButton *btn_del_    = nullptr;
    QToolButton *btn_export_ = nullptr;
    QToolButton *btn_edit_   = nullptr;
    QToolButton *btn_scene_  = nullptr;
    QLabel       *template_lbl_ = nullptr;
    QLabel       *status_lbl_ = nullptr;
    QLabel       *text_editor_lbl_ = nullptr;
    QTableWidget *text_table_ = nullptr;
    QToolButton *btn_add_text_row_ = nullptr;
    QToolButton *btn_delete_text_row_ = nullptr;
    QToolButton *btn_row_up_ = nullptr;
    QToolButton *btn_row_down_ = nullptr;
    bool          updating_exposed_text_ = false;
    std::map<int, QByteArray> live_text_header_states_;
    QTimer       *live_refresh_timer_ = nullptr;
    uint64_t      seen_store_revision_ = 0;

    TitleEditor  *editor_     = nullptr;
};
