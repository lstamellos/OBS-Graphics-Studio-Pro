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
#include <QDateTime>
#include <map>
#include <vector>

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
    void on_toggle_template_view();
    void on_selection_changed();
    void on_add_live_text_row();
    void on_delete_live_text_rows();
    void on_move_live_text_row_up();
    void on_move_live_text_row_down();
    void on_import_live_text_data();
    void on_import_append_live_text_data();
    void on_export_live_text_data();
    void on_toggle_external_data_source();
    void on_show_external_data_settings();
    void on_refresh_external_data();
    void on_toggle_playlist(bool enabled);
    void on_playlist_tick();

private:
    void build_ui();
    void populate_list();
    void populate_exposed_text();
    void update_template_view_mode();
    void set_all_live_text_rows_checked(bool checked);
    void update_live_text_select_all_state();
    void save_live_text_header_state();
    bool restore_live_text_header_state();
    void load_dock_settings();
    void save_dock_settings() const;
    bool cue_live_text_row(int row, bool allow_uncue);
    int live_text_playlist_row_count(const std::shared_ptr<Title> &title) const;
    void start_playlist_step();
    int next_playlist_row(int current_row, int row_count) const;
    int playlist_step_delay_ms(const std::shared_ptr<Title> &title) const;
    int playlist_hold_delay_ms() const;
    bool playlist_row_is_terminal(int row, int row_count) const;
    void play_playlist_outro();
    void update_playlist_controls();
    void update_persistence_controls();
    void update_external_data_controls();
    void apply_persistence_settings_to_title(const std::shared_ptr<Title> &title);
    void update_playlist_countdown_label();
    void stop_playlist();
    bool has_checked_live_text_rows() const;
    void apply_live_text_row_selection(const std::vector<int> &rows, bool checked);
    std::string selected_id() const;
    std::vector<std::string> selected_title_ids() const;
    std::shared_ptr<Title> create_template_title(const std::string &name, int template_id);
    void select_title(const std::string &id);
    void create_title_from_template(const std::string &name, int template_id);
    std::vector<int> selected_live_text_rows() const;

    QWidget      *container_  = nullptr;
    QSplitter    *sections_   = nullptr;
    QListWidget  *list_       = nullptr;
    QToolButton *btn_add_    = nullptr;
    QToolButton *btn_dup_    = nullptr;
    QToolButton *btn_rename_ = nullptr;
    QToolButton *btn_del_    = nullptr;
    QToolButton *btn_export_ = nullptr;
    QToolButton *btn_edit_   = nullptr;
    QToolButton *btn_scene_  = nullptr;
    QToolButton *btn_view_   = nullptr;
    QLabel       *template_lbl_ = nullptr;
    QLabel       *status_lbl_ = nullptr;
    QLabel       *text_editor_lbl_ = nullptr;
    QLabel       *playlist_countdown_lbl_ = nullptr;
    QTableWidget *text_table_ = nullptr;
    QToolButton *btn_add_text_row_ = nullptr;
    QToolButton *btn_delete_text_row_ = nullptr;
    QToolButton *btn_row_up_ = nullptr;
    QToolButton *btn_row_down_ = nullptr;
    QToolButton *btn_data_sources_ = nullptr;
    QToolButton *btn_external_refresh_ = nullptr;
    QToolButton *btn_playlist_ = nullptr;
    QToolButton *btn_playlist_settings_ = nullptr;
    QToolButton *btn_persistence_settings_ = nullptr;
    QAction     *act_playlist_loop_ = nullptr;
    QAction     *act_playlist_reverse_ = nullptr;
    QAction     *act_background_persistence_ = nullptr;
    QAction     *act_text_persistence_ = nullptr;
    bool          updating_exposed_text_ = false;
    bool          template_icon_view_ = false;
    std::map<int, QByteArray> live_text_header_states_;
    QTimer       *live_refresh_timer_ = nullptr;
    QTimer       *playlist_timer_ = nullptr;
    qint64        playlist_next_due_ms_ = 0;
    bool          playlist_stop_after_due_ = false;
    int           playlist_next_row_ = 0;
    double        playlist_hold_seconds_ = 5.0;
    bool          playlist_loop_ = false;
    bool          playlist_reverse_ = false;
    bool          background_persistence_ = false;
    bool          text_persistence_ = false;
    uint64_t      seen_store_revision_ = 0;

    TitleEditor  *editor_     = nullptr;
};
