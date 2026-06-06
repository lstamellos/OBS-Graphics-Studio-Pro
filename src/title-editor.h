/*
 * title-editor.h
 *
 * Part 3: After Effects-style title editor.
 *
 * Layout
 * ┌───────────────────────────────────────────────────────────────┐
 * │  Menu bar  [File ▾]  [Title name]                             │
 * ├──────────────────────────┬────────────────────────────────────┤
 * │  CANVAS PREVIEW          │  PROPERTIES PANEL                  │
 * │  (live render, zoom)     │  (layer-specific controls)         │
 * │                          │                                    │
 * ├──────────────────────────┴────────────────────────────────────┤
 * │  LAYER STACK                │  TIMELINE / KEYFRAME EDITOR      │
 * │  (AE-style layer list)      │  (ruler, clips, keyframe dots)   │
 * └─────────────────────────────┴──────────────────────────────────┘
 *
 * The editor is a QDialog (non-modal so OBS stays usable).
 */

#pragma once

#include "title-data.h"
#include <QDialog>
#include <QSplitter>
#include <QListWidget>
#include <QToolBar>
#include <QLabel>
#include <QScrollArea>
#include <QSlider>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QComboBox>
#include <QCheckBox>
#include <QGroupBox>
#include <QFormLayout>
#include <QTimer>
#include <QElapsedTimer>
#include <QPushButton>
#include <QLineEdit>
#include <QTextEdit>
#include <QSpinBox>
#include <QPointF>
#include <QPoint>
#include <QRectF>
#include <memory>
#include <string>
#include <vector>
#include <set>

/* Forward declarations for sub-widgets */
class CanvasPreview;
class LayerStack;
class TimelineWidget;
class PropertiesPanel;
class TitlePropertiesPanel;
class QEvent;
class QKeyEvent;
class QContextMenuEvent;
class QResizeEvent;
class QCloseEvent;
class QAction;
class QToolButton;
class QScrollBar;

/* ══════════════════════════════════════════════════════════════════
 *  TitleEditor  – main editor window
 * ══════════════════════════════════════════════════════════════════ */
class TitleEditor : public QDialog {
    Q_OBJECT

public:
    explicit TitleEditor(QWidget *parent = nullptr);
    ~TitleEditor() override = default;

    void open_title(const std::string &title_id);

signals:
    void title_saved(const std::string &title_id);

public slots:
    /* Transport */
    void play_pause();
    void play_full_loop();
    void rewind();
    void step_forward();
    void previous_keyframe();
    void next_keyframe();

    /* Called by sub-widgets */
    void on_layer_selected(const std::string &layer_id);
    void on_playhead_changed(double t);
    void on_title_modified(bool push_undo_snapshot = true);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void keyPressEvent(QKeyEvent *ev) override;
    void closeEvent(QCloseEvent *ev) override;

private slots:
    void tick();
    void show_about();
    void reject() override;

private:
    void build_ui();
    void build_toolbar();
    void update_title_bar();
    void set_dirty(bool dirty);
    bool confirm_save_before_close();
    void new_title_contents();
    bool save_title();
    bool persist_title_changes(bool update_preview_screenshot, bool show_saved_status);
    void set_live_editing_enabled(bool enabled);
    void save_live_edit();
    void save_title_as_new();
    void export_title_template(bool save_in_library);
    void copy_title_to_store(const std::shared_ptr<Title> &source, const std::shared_ptr<Title> &dest) const;
    void align_selected_to_canvas(int x_mode, int y_mode);
    void align_selected_layers_horizontal();
    void align_selected_layers_vertical();
    void align_selected_layers(int x_mode, int y_mode);
    void flip_selected_layers(bool horizontal);
    void rotate_selected_layers(double degrees);
    std::shared_ptr<Title> clone_title(const Title &title) const;
    std::shared_ptr<Layer> clone_layer_for_insert(const Layer &layer, bool suffix_name) const;
    void insert_layer_above(const std::string &anchor_id, std::shared_ptr<Layer> layer);
    void select_after_layer_list_mutation(const std::string &layer_id);
    void copy_selected_layer();
    void cut_selected_layer();
    void paste_layer_from_clipboard();
    void delete_selected_layer();
    void push_undo_snapshot();
    void restore_undo_snapshot(int index);
    void update_undo_redo_actions();

    /* Current editing state */
    std::shared_ptr<Title> title_;
    std::string            editing_title_id_;
    std::string            sel_layer_id_;
    double                 playhead_  = 0.0;
    bool                   playing_   = false;
    bool                   playback_reverse_ = false;
    bool                   full_loop_playback_ = false;
    bool                   dirty_ = false;
    QTimer                *play_timer_ = nullptr;
    QTimer                *clock_timer_ = nullptr;
    QElapsedTimer          playback_clock_;

    /* Sub-widgets */
    CanvasPreview   *canvas_    = nullptr;
    LayerStack      *layers_    = nullptr;
    TimelineWidget  *timeline_  = nullptr;
    PropertiesPanel *props_     = nullptr;
    TitlePropertiesPanel *title_props_ = nullptr;
    QLabel          *time_lbl_  = nullptr;
    QLabel          *title_lbl_ = nullptr;
    QLabel          *dirty_indicator_ = nullptr;

    QToolBar        *toolbar_   = nullptr;
    QAction         *act_play_  = nullptr;
    QAction         *act_full_loop_ = nullptr;
    QAction         *act_rew_   = nullptr;
    QAction         *act_prev_kf_ = nullptr;
    QAction         *act_next_kf_ = nullptr;
    QAction         *act_safe_guides_ = nullptr;
    QAction         *act_live_editing_ = nullptr;
    QAction         *act_undo_ = nullptr;
    QAction         *act_redo_ = nullptr;
    int              alignment_target_ = 3; /* 0=selection, 1=title safe guides, 2=action safe guides, 3=artboard/canvas */
    std::vector<std::shared_ptr<Title>> undo_stack_;
    int              undo_index_ = -1;
    bool             restoring_undo_ = false;
    bool             live_editing_ = false;
    std::shared_ptr<Layer> layer_clipboard_;
};

/* ══════════════════════════════════════════════════════════════════
 *  CanvasPreview  – renders the title at the current playhead
 * ══════════════════════════════════════════════════════════════════ */
class CanvasPreview : public QWidget {
    Q_OBJECT

public:
    explicit CanvasPreview(QWidget *parent = nullptr);

    void set_title(std::shared_ptr<Title> t);
    void set_playhead(double t);
    void set_selected_layer(const std::string &lid);
    void set_selected_layers(const std::vector<std::string> &ids);
    void set_safe_guides_visible(bool visible);
    void set_snap_enabled(bool enabled);
    void set_snap_to_guides(bool enabled);
    void set_snap_to_grid(bool enabled);
    void set_snap_to_object_edges(bool enabled);
    void set_snap_to_object_centers(bool enabled);
    void set_snap_to_canvas_bounds(bool enabled);
    void set_snap_to_spacing(bool enabled);
    void refresh_preview();
    void set_zoom_percent(int percent);
    int zoom_percent() const;
    void fit_canvas(bool up_to_100 = false);
    void set_checkerboard_pattern(int pattern);

signals:
    void layer_clicked(const std::string &layer_id);
    void layers_selected(const std::vector<std::string> &layer_ids);
    void layer_geometry_changed();
    void layer_structure_changed();
    void zoom_percent_changed(int percent);

protected:
    void paintEvent(QPaintEvent *ev) override;
    void mousePressEvent(QMouseEvent *ev) override;
    void mouseMoveEvent(QMouseEvent *ev) override;
    void mouseReleaseEvent(QMouseEvent *ev) override;
    void keyPressEvent(QKeyEvent *ev) override;
    void wheelEvent(QWheelEvent *ev) override;
    void resizeEvent(QResizeEvent *ev) override;

private:
    enum class DragMode { None, Marquee, Move, ResizeNW, ResizeN, ResizeNE, ResizeE, ResizeSE, ResizeS, ResizeSW, ResizeW, Origin, Rotate };

    void render_to_pixmap();
    std::shared_ptr<Layer> selected_layer() const;
    std::vector<std::shared_ptr<Layer>> selected_layers() const;
    QRectF layer_local_rect(const Layer &layer) const;
    double fit_scale() const;
    double view_scale() const;
    QPointF centered_view_origin() const;
    QPointF view_origin() const;
    QPointF view_to_canvas(const QPointF &view_pt) const;
    QPointF canvas_to_view(const QPointF &canvas_pt) const;
    QPointF canvas_to_layer(const Layer &layer, const QPointF &canvas_pt) const;
    QPointF layer_to_canvas(const Layer &layer, const QPointF &layer_pt) const;
    DragMode hit_test_selected(const QPointF &view_pt) const;
    QRectF layer_canvas_bounds(const Layer &layer) const;
    QRectF selected_canvas_bounds() const;
    void begin_marquee(const QPointF &view_pt, Qt::KeyboardModifiers modifiers);
    void update_marquee(const QPointF &view_pt, Qt::KeyboardModifiers modifiers);
    bool duplicate_selected_layers_for_drag();
    bool nudge_selected_layers(double dx, double dy);
    QPointF snap_delta_for_bounds(const QRectF &start_bounds, const QPointF &delta, bool snap_x, bool snap_y);
    QPointF snap_canvas_point(const QPointF &canvas_pt, bool snap_x, bool snap_y);
    void collect_snap_targets(bool x_axis, std::vector<double> &targets, std::vector<QString> &labels) const;
    void collect_spacing_targets(bool x_axis, std::vector<double> &targets, std::vector<QString> &labels) const;
    void clear_snap_feedback();
    void add_snap_feedback(bool x_axis, double value, const QString &label);
    void apply_drag(const QPointF &view_pt, Qt::KeyboardModifiers modifiers = Qt::NoModifier);

    std::shared_ptr<Title> title_;
    std::string sel_layer_id_;
    std::vector<std::string> selected_layer_ids_;
    double playhead_ = 0.0;
    int zoom_percent_ = 100;
    bool fit_zoom_active_ = true;
    bool fit_zoom_up_to_100_ = false;
    QPointF pan_offset_;
    bool panning_ = false;
    QPointF pan_start_view_;
    QPointF pan_start_offset_;
    QPixmap frame_pixmap_;
    bool dirty_ = true;
    bool safe_guides_visible_ = false;
    int checkerboard_pattern_ = 1;

    struct SnapSettings {
        bool enabled = true;
        bool guides = true;
        bool grid = false;
        bool object_edges = true;
        bool object_centers = true;
        bool canvas_bounds = true;
        bool spacing = true;
    };
    struct SnapFeedback {
        bool x_axis = true;
        double value = 0.0;
        QString label;
    };
    SnapSettings snap_settings_;
    std::vector<SnapFeedback> snap_feedback_;

    DragMode drag_mode_ = DragMode::None;
    bool drag_changed_ = false;
    bool alt_duplicate_pending_ = false;
    bool alt_duplicate_done_ = false;
    bool drag_text_object_scaling_ = false;
    bool marquee_active_ = false;
    QPointF drag_start_view_;
    QPointF drag_current_view_;
    std::vector<std::string> marquee_base_selection_;
    QPointF drag_start_canvas_;
    QPointF drag_rotation_pivot_canvas_;
    double drag_start_rotation_angle_ = 0.0;
    double drag_current_rotation_delta_ = 0.0;
    double drag_start_x_ = 0.0;
    double drag_start_y_ = 0.0;
    float drag_start_w_ = 1.0f;
    float drag_start_h_ = 1.0f;
    float drag_start_origin_x_ = 0.5f;
    float drag_start_origin_y_ = 0.5f;
    QRectF drag_start_selection_bounds_;
    struct LayerDragState {
        std::string id;
        double x = 0.0;
        double y = 0.0;
        float w = 1.0f;
        float h = 1.0f;
        double scale_x = 1.0;
        double scale_y = 1.0;
        double rotation = 0.0;
    };
    std::vector<LayerDragState> drag_layer_states_;
};

/* ══════════════════════════════════════════════════════════════════
 *  LayerStack  – AE-style layer list on the left of the timeline
 * ══════════════════════════════════════════════════════════════════ */
class LayerStack : public QWidget {
    Q_OBJECT

public:
    explicit LayerStack(QWidget *parent = nullptr);

    void set_title(std::shared_ptr<Title> t);
    void refresh();
    void set_selected_layer(const std::string &layer_id);
    void set_selected_layers(const std::vector<std::string> &layer_ids);
    void set_layer_clipboard_available(bool available);
    QScrollBar *vertical_scroll_bar() const;
    std::vector<std::string> selected_ids() const;

signals:
    void layer_selected(const std::string &layer_id);
    void layers_selected(const std::vector<std::string> &layer_ids);
    void layer_visibility_changed(const std::string &layer_id, bool v);
    void layer_lock_changed(const std::string &layer_id, bool locked);
    void layer_expand_changed(const std::string &layer_id, bool expanded);
    void layer_parent_changed(const std::string &layer_id, const std::string &parent_id);
    void layer_name_changed(const std::string &layer_id, const std::string &name);
    void layer_order_changed();
    void add_layer_requested(LayerType type);
    void clone_layer_requested(const std::string &layer_id);
    void copy_layer_requested(const std::string &layer_id);
    void paste_layer_requested(const std::string &layer_id);
    void delete_layer_requested(const std::string &layer_id);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private slots:
    void on_add_text();
    void on_add_clock();
    void on_add_ticker();
    void on_add_rect();
    void on_add_image();
    void on_move_up();
    void on_move_down();
    void on_delete();
    void on_item_changed(QListWidgetItem *item);
    void on_selection_changed();
    void show_layer_context_menu(const QPoint &pos);

private:
    void populate();
    void sync_order_from_list();
    std::string selected_id() const;

    std::shared_ptr<Title> title_;
    QListWidget  *list_     = nullptr;
    QToolButton  *btn_add_  = nullptr;
    QToolButton  *btn_move_up_ = nullptr;
    QToolButton  *btn_move_down_ = nullptr;
    QToolButton  *btn_del_       = nullptr;
    bool          layer_clipboard_available_ = false;
};

/* ══════════════════════════════════════════════════════════════════
 *  TimelineWidget  – keyframe timeline
 * ══════════════════════════════════════════════════════════════════ */
class TimelineWidget : public QWidget {
    Q_OBJECT

public:
    explicit TimelineWidget(QWidget *parent = nullptr);

    void set_title(std::shared_ptr<Title> t);
    void set_selected_layer(const std::string &lid);
    void set_playhead(double t);
    void set_vertical_scroll(int scroll_y);
    void set_zoom_percent(int percent);
    int zoom_percent() const;
    void fit_timeline();
    bool has_selected_keyframes() const;
    bool has_keyframe_clipboard() const;
    bool copy_keyframe_selection();
    bool cut_keyframe_selection();
    bool delete_keyframe_selection();
    bool paste_keyframes_at_playhead();

signals:
    void playhead_changed(double t);
    void keyframe_added(const std::string &layer_id,
                        const std::string &prop_name, double t);
    void keyframe_moved(const std::string &layer_id,
                        const std::string &prop_name, int kf_idx, double new_t);
    void keyframe_easing_changed();
    void vertical_scroll_delta_requested(int delta);
    void zoom_percent_changed(int percent);
    void layer_selected(const std::string &layer_id);

protected:
    void paintEvent(QPaintEvent *ev) override;
    void mousePressEvent(QMouseEvent *ev) override;
    void mouseMoveEvent(QMouseEvent *ev) override;
    void mouseReleaseEvent(QMouseEvent *ev) override;
    void keyPressEvent(QKeyEvent *ev) override;
    void contextMenuEvent(QContextMenuEvent *ev) override;
    void wheelEvent(QWheelEvent *ev) override;
    void resizeEvent(QResizeEvent *ev) override;

private:
    double x_to_time(int x) const;
    int    time_to_x(double t) const;
    int    ruler_height() const { return 72; }
    int    row_height()   const { return 28; }
    double snap_time(double t) const;
    void   clamp_scroll();
    void   clamp_vertical_scroll();
    int    max_vertical_scroll() const;
    bool   hit_keyframe(const QPoint &pos, std::shared_ptr<Layer> *layer,
                        AnimatedProperty **prop, int *kf_idx, int *row_idx) const;
    struct KeyframeRef {
        std::string layer_id;
        std::string prop_name;
        int index = -1;
        bool operator<(const KeyframeRef &other) const;
    };
    struct DraggedKeyframe {
        KeyframeRef ref;
        double start_time = 0.0;
    };
    struct ClipboardKeyframe {
        std::string layer_id;
        std::string prop_name;
        Keyframe keyframe;
        double offset = 0.0;
    };
    void   clear_keyframe_selection();
    void   prune_keyframe_selection();
    bool   is_keyframe_selected(const std::string &layer_id, const std::string &prop_name, int kf_idx) const;
    void   select_keyframe(const std::string &layer_id, const std::string &prop_name, int kf_idx, bool additive, bool toggle);
    void   select_keyframes_in_rect(const QRect &rect, bool additive);
    bool   copy_selected_keyframes();
    bool   delete_selected_keyframes();
    bool   cut_selected_keyframes();
    bool   paste_keyframes_at(double timeline_time);
    QRect  marquee_rect() const;
    void   begin_keyframe_drag(const std::string &layer_id, const std::string &prop_name, int kf_idx, double start_time);
    AnimatedProperty *find_timeline_property(Layer &layer, const std::string &prop_name) const;
    bool   keep_playhead_visible();
    void   set_pixels_per_sec(double pixels_per_sec, double anchor_time, int anchor_x);

    enum class DragMode { None, Playhead, Keyframe, Marquee, TrimIn, TrimOut, Layer, LoopStart, LoopEnd, PauseMarker };

    std::shared_ptr<Title> title_;
    std::string sel_layer_id_;
    bool fit_on_next_resize_ = false;
    double playhead_  = 0.0;
    DragMode drag_mode_ = DragMode::None;
    std::string drag_layer_id_;
    std::string drag_prop_name_;
    int drag_keyframe_index_ = -1;
    double drag_start_time_ = 0.0;
    double drag_start_in_ = 0.0;
    double drag_start_out_ = 0.0;
    std::set<KeyframeRef> selected_keyframes_;
    std::vector<DraggedKeyframe> dragged_keyframes_;
    std::vector<ClipboardKeyframe> keyframe_clipboard_;
    QPoint marquee_start_;
    QPoint marquee_current_;
    bool marquee_additive_ = false;
    bool marquee_moved_ = false;
    double pixels_per_sec_ = 80.0;
    int    scroll_x_       = 0;
    int    scroll_y_       = 0;
};

/* ══════════════════════════════════════════════════════════════════
 *  TitlePropertiesPanel – global title inspector
 * ══════════════════════════════════════════════════════════════════ */
class TitlePropertiesPanel : public QGroupBox {
    Q_OBJECT

public:
    explicit TitlePropertiesPanel(QWidget *parent = nullptr);
    void set_title(std::shared_ptr<Title> t);

signals:
    void title_changed(bool push_undo_snapshot = true);

private:
    void load_values();

    std::shared_ptr<Title> title_;
    bool loading_values_ = false;
    bool numeric_label_dragging_ = false;
    QComboBox      *cmb_playback_mode_ = nullptr;
    QComboBox      *cmb_loop_type_ = nullptr;
    QSpinBox       *spn_pause_frame_ = nullptr;
    QDoubleSpinBox *spn_duration_ = nullptr;
    QDoubleSpinBox *spn_loop_start_ = nullptr;
    QDoubleSpinBox *spn_loop_end_ = nullptr;
};

/* ══════════════════════════════════════════════════════════════════
 *  PropertiesPanel  – right-side inspector
 * ══════════════════════════════════════════════════════════════════ */
class PropertiesPanel : public QScrollArea {
    Q_OBJECT

public:
    explicit PropertiesPanel(QWidget *parent = nullptr);

    void set_layer(std::shared_ptr<Layer> layer, double playhead);
    void set_title(std::shared_ptr<Title> t);

signals:
    void property_changed(bool push_undo_snapshot = true);

private:
    void build_text_section(QWidget *w, QFormLayout *fl);
    void build_rect_section(QWidget *w, QFormLayout *fl);
    void build_transform_section(QWidget *w, QFormLayout *fl);

    void load_values();

    std::shared_ptr<Layer> layer_;
    std::shared_ptr<Title> title_;
    double playhead_ = 0.0;
    bool loading_values_ = false;
    bool numeric_label_dragging_ = false;

    QGroupBox       *text_box_     = nullptr;
    QGroupBox       *type_options_box_ = nullptr;
    QGroupBox       *paragraph_box_ = nullptr;
    QGroupBox       *dynamic_text_box_ = nullptr;
    QGroupBox       *bullets_box_ = nullptr;
    QGroupBox       *rect_box_     = nullptr;
    QGroupBox       *image_box_    = nullptr;

    /* Text controls */
    QTextEdit       *txt_content_  = nullptr;
    QComboBox       *cmb_font_     = nullptr;
    QComboBox       *cmb_font_style_ = nullptr;
    QSpinBox        *spn_size_     = nullptr;
    QToolButton     *chk_bold_     = nullptr;
    QToolButton     *chk_italic_   = nullptr;
    QToolButton     *chk_font_kerning_ = nullptr;
    QComboBox       *cmb_kerning_mode_ = nullptr;
    QDoubleSpinBox  *spn_kerning_value_ = nullptr;
    QDoubleSpinBox  *spn_text_leading_ = nullptr;
    QDoubleSpinBox  *spn_char_tracking_ = nullptr;
    QDoubleSpinBox  *spn_char_scale_x_ = nullptr;
    QDoubleSpinBox  *spn_char_scale_y_ = nullptr;
    QDoubleSpinBox  *spn_baseline_shift_ = nullptr;
    QComboBox       *cmb_language_ = nullptr;
    QComboBox       *cmb_text_style_ = nullptr;
    QToolButton     *btn_all_caps_ = nullptr;
    QToolButton     *btn_small_caps_ = nullptr;
    QToolButton     *btn_superscript_ = nullptr;
    QToolButton     *btn_subscript_ = nullptr;
    QToolButton     *btn_underline_ = nullptr;
    QToolButton     *btn_strikethrough_ = nullptr;
    QToolButton     *btn_ligatures_ = nullptr;
    QToolButton     *btn_stylistic_alternates_ = nullptr;
    QToolButton     *btn_fractions_ = nullptr;
    QToolButton     *btn_opentype_features_ = nullptr;
    QComboBox       *cmb_text_overflow_ = nullptr;
    QDoubleSpinBox  *spn_text_fit_min_scale_ = nullptr;
    QComboBox       *cmb_ticker_style_ = nullptr;
    QDoubleSpinBox  *spn_ticker_speed_ = nullptr;
    QDoubleSpinBox  *spn_ticker_line_hold_ = nullptr;
    QComboBox       *cmb_ticker_direction_ = nullptr;
    QLabel          *lbl_text_fit_scale_ = nullptr;
    QCheckBox       *chk_text_box_width_to_text_ = nullptr;
    QCheckBox       *chk_text_box_height_to_text_ = nullptr;
    QDoubleSpinBox  *spn_max_text_box_width_ = nullptr;
    QDoubleSpinBox  *spn_max_text_box_height_ = nullptr;
    QCheckBox       *chk_expose_text_ = nullptr;
    QComboBox       *cmb_text_align_ = nullptr;
    QComboBox       *cmb_text_valign_ = nullptr;
    QPushButton     *btn_text_color_ = nullptr;

    /* Text/shape outline controls */
    QGroupBox       *outline_box_ = nullptr;
    QCheckBox       *chk_outline_enabled_ = nullptr;
    QDoubleSpinBox  *spn_outline_width_ = nullptr;
    QPushButton     *btn_outline_color_ = nullptr;
    QWidget         *row_outline_color_ = nullptr;
    QDoubleSpinBox  *spn_outline_opacity_ = nullptr;
    QComboBox       *cmb_outline_join_ = nullptr;
    QComboBox       *cmb_outline_position_ = nullptr;
    QCheckBox       *chk_outline_antialias_ = nullptr;

    /* Rectangle/Image geometry controls */
    QDoubleSpinBox  *spn_layer_w_   = nullptr;
    QDoubleSpinBox  *spn_layer_h_   = nullptr;
    QDoubleSpinBox  *spn_rect_corner_   = nullptr;
    QPushButton     *btn_fill_color_ = nullptr;
    QWidget         *row_fill_color_ = nullptr;
    QComboBox       *cmb_fill_type_ = nullptr;
    QWidget         *row_fill_type_ = nullptr;
    QGroupBox       *gradient_box_ = nullptr;
    QComboBox       *cmb_gradient_type_ = nullptr;
    QPushButton     *btn_gradient_start_color_ = nullptr;
    QPushButton     *btn_gradient_end_color_ = nullptr;
    QDoubleSpinBox  *spn_gradient_start_pos_ = nullptr;
    QDoubleSpinBox  *spn_gradient_end_pos_ = nullptr;
    QDoubleSpinBox  *spn_gradient_start_opacity_ = nullptr;
    QDoubleSpinBox  *spn_gradient_end_opacity_ = nullptr;
    QDoubleSpinBox  *spn_gradient_opacity_ = nullptr;
    QDoubleSpinBox  *spn_gradient_angle_ = nullptr;
    QDoubleSpinBox  *spn_gradient_center_x_ = nullptr;
    QDoubleSpinBox  *spn_gradient_center_y_ = nullptr;
    QDoubleSpinBox  *spn_gradient_scale_ = nullptr;
    QDoubleSpinBox  *spn_gradient_focal_x_ = nullptr;
    QDoubleSpinBox  *spn_gradient_focal_y_ = nullptr;

    /* Text/image background controls */
    QCheckBox       *chk_background_enabled_ = nullptr;
    QPushButton     *btn_background_color_ = nullptr;
    QDoubleSpinBox  *spn_background_opacity_ = nullptr;
    QDoubleSpinBox  *spn_background_padding_x_ = nullptr;
    QDoubleSpinBox  *spn_background_padding_y_ = nullptr;
    QDoubleSpinBox  *spn_background_corner_ = nullptr;
    QWidget         *row_background_enabled_ = nullptr;
    QWidget         *row_background_color_ = nullptr;
    QWidget         *row_background_opacity_ = nullptr;
    QWidget         *row_background_padding_x_ = nullptr;
    QWidget         *row_background_padding_y_ = nullptr;
    QWidget         *row_background_corner_ = nullptr;

    /* Image controls */
    QLineEdit       *edit_image_path_ = nullptr;
    QPushButton     *btn_pick_image_ = nullptr;

    /* Transform controls (static) */
    QDoubleSpinBox  *spn_px_       = nullptr;
    QDoubleSpinBox  *spn_py_       = nullptr;
    QDoubleSpinBox  *spn_scale_x_  = nullptr;
    QDoubleSpinBox  *spn_scale_y_  = nullptr;
    QDoubleSpinBox  *spn_rot_      = nullptr;
    QDoubleSpinBox  *spn_opacity_  = nullptr;
    QDoubleSpinBox  *spn_origin_x_ = nullptr;
    QDoubleSpinBox  *spn_origin_y_ = nullptr;
    QCheckBox       *chk_scale_lock_ = nullptr;
    QCheckBox       *chk_lock_aspect_ = nullptr;
    QComboBox       *cmb_anchor_ = nullptr;
    QGroupBox       *shadow_box_ = nullptr;
    QCheckBox       *chk_shadow_enabled_ = nullptr;
    QComboBox       *cmb_shadow_preset_ = nullptr;
    QPushButton     *btn_shadow_color_ = nullptr;
    QDoubleSpinBox  *spn_shadow_opacity_ = nullptr;
    QDoubleSpinBox  *spn_shadow_distance_ = nullptr;
    QDoubleSpinBox  *spn_shadow_angle_ = nullptr;
    QDoubleSpinBox  *spn_shadow_blur_ = nullptr;
    QDoubleSpinBox  *spn_shadow_spread_ = nullptr;
    QPushButton     *btn_kf_shadow_enabled_ = nullptr;
    QPushButton     *btn_kf_shadow_color_ = nullptr;
    QPushButton     *btn_kf_shadow_opacity_ = nullptr;
    QPushButton     *btn_kf_shadow_distance_ = nullptr;
    QPushButton     *btn_kf_shadow_angle_ = nullptr;
    QPushButton     *btn_kf_shadow_blur_ = nullptr;
    QPushButton     *btn_kf_shadow_spread_ = nullptr;
    QPushButton     *btn_kf_pos_x_ = nullptr;
    QPushButton     *btn_kf_pos_y_ = nullptr;
    QPushButton     *btn_kf_scale_x_ = nullptr;
    QPushButton     *btn_kf_scale_y_ = nullptr;
    QPushButton     *btn_kf_rotation_ = nullptr;
    QPushButton     *btn_kf_opacity_ = nullptr;
    QPushButton     *btn_kf_origin_x_ = nullptr;
    QPushButton     *btn_kf_origin_y_ = nullptr;
    QPushButton     *btn_kf_width_ = nullptr;
    QPushButton     *btn_kf_height_ = nullptr;
    QPushButton     *btn_kf_text_color_ = nullptr;
    QPushButton     *btn_kf_fill_color_ = nullptr;
    QPushButton     *btn_kf_background_enabled_ = nullptr;
    QPushButton     *btn_kf_background_color_ = nullptr;
    QPushButton     *btn_kf_background_opacity_ = nullptr;
    QPushButton     *btn_kf_background_padding_x_ = nullptr;
    QPushButton     *btn_kf_background_padding_y_ = nullptr;
    QPushButton     *btn_kf_background_corner_ = nullptr;
};
