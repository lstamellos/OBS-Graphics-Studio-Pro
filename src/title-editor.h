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
#include <QSpinBox>
#include <QPointF>
#include <QPoint>
#include <QRectF>
#include <memory>

/* Forward declarations for sub-widgets */
class CanvasPreview;
class LayerStack;
class TimelineWidget;
class PropertiesPanel;
class TitlePropertiesPanel;
class QKeyEvent;
class QContextMenuEvent;
class QAction;

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
    void on_title_modified();

protected:
    void keyPressEvent(QKeyEvent *ev) override;

private slots:
    void tick();

private:
    void build_ui();
    void build_toolbar();
    void update_title_bar();
    void align_selected_to_canvas(int x_mode, int y_mode);
    void align_selected_layers_horizontal();
    void align_selected_layers_vertical();
    void align_selected_layers(int x_mode, int y_mode);
    std::shared_ptr<Title> clone_title(const Title &title) const;
    void push_undo_snapshot();
    void restore_undo_snapshot(int index);
    void update_undo_redo_actions();

    /* Current editing state */
    std::shared_ptr<Title> title_;
    std::string            sel_layer_id_;
    double                 playhead_  = 0.0;
    bool                   playing_   = false;
    bool                   playback_reverse_ = false;
    bool                   full_loop_playback_ = false;
    QTimer                *play_timer_ = nullptr;
    QElapsedTimer          playback_clock_;

    /* Sub-widgets */
    CanvasPreview   *canvas_    = nullptr;
    LayerStack      *layers_    = nullptr;
    TimelineWidget  *timeline_  = nullptr;
    PropertiesPanel *props_     = nullptr;
    TitlePropertiesPanel *title_props_ = nullptr;
    QLabel          *time_lbl_  = nullptr;
    QLabel          *title_lbl_ = nullptr;

    QToolBar        *toolbar_   = nullptr;
    QAction         *act_play_  = nullptr;
    QAction         *act_full_loop_ = nullptr;
    QAction         *act_rew_   = nullptr;
    QAction         *act_prev_kf_ = nullptr;
    QAction         *act_next_kf_ = nullptr;
    QAction         *act_safe_guides_ = nullptr;
    QAction         *act_undo_ = nullptr;
    QAction         *act_redo_ = nullptr;
    int              alignment_target_ = 2; /* 0=selection, 2=artboard/canvas */
    std::vector<std::shared_ptr<Title>> undo_stack_;
    int              undo_index_ = -1;
    bool             restoring_undo_ = false;
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
    void set_safe_guides_visible(bool visible);
    void refresh_preview();

signals:
    void layer_clicked(const std::string &layer_id);
    void layer_geometry_changed();

protected:
    void paintEvent(QPaintEvent *ev) override;
    void mousePressEvent(QMouseEvent *ev) override;
    void mouseMoveEvent(QMouseEvent *ev) override;
    void mouseReleaseEvent(QMouseEvent *ev) override;
    void wheelEvent(QWheelEvent *ev) override;
    void resizeEvent(QResizeEvent *ev) override;

private:
    enum class DragMode { None, Move, ResizeNW, ResizeN, ResizeNE, ResizeE, ResizeSE, ResizeS, ResizeSW, ResizeW, Origin };

    void render_to_pixmap();
    std::shared_ptr<Layer> selected_layer() const;
    QRectF layer_local_rect(const Layer &layer) const;
    double view_scale() const;
    QPointF view_origin() const;
    QPointF view_to_canvas(const QPointF &view_pt) const;
    QPointF canvas_to_view(const QPointF &canvas_pt) const;
    QPointF canvas_to_layer(const Layer &layer, const QPointF &canvas_pt) const;
    QPointF layer_to_canvas(const Layer &layer, const QPointF &layer_pt) const;
    DragMode hit_test_selected(const QPointF &view_pt) const;
    void apply_drag(const QPointF &view_pt, Qt::KeyboardModifiers modifiers = Qt::NoModifier);

    std::shared_ptr<Title> title_;
    std::string sel_layer_id_;
    double playhead_ = 0.0;
    float  zoom_     = 1.0f;
    QPixmap frame_pixmap_;
    bool dirty_ = true;
    bool safe_guides_visible_ = false;

    DragMode drag_mode_ = DragMode::None;
    bool drag_changed_ = false;
    QPointF drag_start_canvas_;
    double drag_start_x_ = 0.0;
    double drag_start_y_ = 0.0;
    float drag_start_w_ = 1.0f;
    float drag_start_h_ = 1.0f;
    float drag_start_origin_x_ = 0.5f;
    float drag_start_origin_y_ = 0.5f;
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
    std::vector<std::string> selected_ids() const;

signals:
    void layer_selected(const std::string &layer_id);
    void layer_visibility_changed(const std::string &layer_id, bool v);
    void layer_lock_changed(const std::string &layer_id, bool locked);
    void layer_expand_changed(const std::string &layer_id, bool expanded);
    void layer_parent_changed(const std::string &layer_id, const std::string &parent_id);
    void layer_name_changed(const std::string &layer_id, const std::string &name);
    void layer_order_changed();
    void add_layer_requested(LayerType type);
    void delete_layer_requested(const std::string &layer_id);

private slots:
    void on_add_text();
    void on_add_rect();
    void on_add_image();
    void on_delete();
    void on_item_changed(QListWidgetItem *item);
    void on_selection_changed();

private:
    void populate();
    void sync_order_from_list();
    std::string selected_id() const;

    std::shared_ptr<Title> title_;
    QListWidget  *list_     = nullptr;
    QPushButton  *btn_add_text_  = nullptr;
    QPushButton  *btn_add_rect_  = nullptr;
    QPushButton  *btn_add_image_ = nullptr;
    QPushButton  *btn_del_       = nullptr;
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

signals:
    void playhead_changed(double t);
    void keyframe_added(const std::string &layer_id,
                        const std::string &prop_name, double t);
    void keyframe_moved(const std::string &layer_id,
                        const std::string &prop_name, int kf_idx, double new_t);
    void keyframe_easing_changed();

protected:
    void paintEvent(QPaintEvent *ev) override;
    void mousePressEvent(QMouseEvent *ev) override;
    void mouseMoveEvent(QMouseEvent *ev) override;
    void mouseReleaseEvent(QMouseEvent *ev) override;
    void contextMenuEvent(QContextMenuEvent *ev) override;
    void wheelEvent(QWheelEvent *ev) override;

private:
    double x_to_time(int x) const;
    int    time_to_x(double t) const;
    int    ruler_height() const { return 72; }
    int    row_height()   const { return 24; }
    double snap_time(double t) const;
    void   clamp_scroll();
    bool   hit_keyframe(const QPoint &pos, std::shared_ptr<Layer> *layer,
                        AnimatedProperty **prop, int *kf_idx, int *row_idx) const;

    enum class DragMode { None, Playhead, Keyframe, TrimIn, TrimOut, Layer, LoopStart, LoopEnd, PauseMarker };

    std::shared_ptr<Title> title_;
    std::string sel_layer_id_;
    double playhead_  = 0.0;
    DragMode drag_mode_ = DragMode::None;
    std::string drag_layer_id_;
    std::string drag_prop_name_;
    int drag_keyframe_index_ = -1;
    double drag_start_time_ = 0.0;
    double drag_start_in_ = 0.0;
    double drag_start_out_ = 0.0;
    double pixels_per_sec_ = 80.0;
    int    scroll_x_       = 0;
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
    void title_changed();

private:
    void load_values();

    std::shared_ptr<Title> title_;
    bool loading_values_ = false;
    QComboBox      *cmb_playback_mode_ = nullptr;
    QComboBox      *cmb_loop_type_ = nullptr;
    QSpinBox       *spn_pause_frame_ = nullptr;
    QDoubleSpinBox *spn_pause_time_ = nullptr;
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
    void property_changed();

private:
    void build_text_section(QWidget *w, QFormLayout *fl);
    void build_rect_section(QWidget *w, QFormLayout *fl);
    void build_transform_section(QWidget *w, QFormLayout *fl);

    void load_values();

    std::shared_ptr<Layer> layer_;
    std::shared_ptr<Title> title_;
    double playhead_ = 0.0;
    bool loading_values_ = false;

    QGroupBox       *text_box_     = nullptr;
    QGroupBox       *rect_box_     = nullptr;
    QGroupBox       *image_box_    = nullptr;

    /* Text controls */
    QLineEdit       *txt_content_  = nullptr;
    QComboBox       *cmb_font_     = nullptr;
    QSpinBox        *spn_size_     = nullptr;
    QCheckBox       *chk_bold_     = nullptr;
    QCheckBox       *chk_italic_   = nullptr;
    QCheckBox       *chk_expose_text_ = nullptr;
    QComboBox       *cmb_text_align_ = nullptr;
    QPushButton     *btn_text_color_ = nullptr;

    /* Rectangle/Image geometry controls */
    QDoubleSpinBox  *spn_layer_w_   = nullptr;
    QDoubleSpinBox  *spn_layer_h_   = nullptr;
    QDoubleSpinBox  *spn_rect_corner_   = nullptr;
    QPushButton     *btn_fill_color_ = nullptr;
    QWidget         *row_fill_color_ = nullptr;

    /* Image controls */
    QLineEdit       *edit_image_path_ = nullptr;
    QPushButton     *btn_pick_image_ = nullptr;

    /* Transform controls (static) */
    QDoubleSpinBox  *spn_px_       = nullptr;
    QDoubleSpinBox  *spn_py_       = nullptr;
    QDoubleSpinBox  *spn_rot_      = nullptr;
    QDoubleSpinBox  *spn_opacity_  = nullptr;
    QDoubleSpinBox  *spn_origin_x_ = nullptr;
    QDoubleSpinBox  *spn_origin_y_ = nullptr;
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
    QPushButton     *btn_kf_rotation_ = nullptr;
    QPushButton     *btn_kf_opacity_ = nullptr;
    QPushButton     *btn_kf_origin_x_ = nullptr;
    QPushButton     *btn_kf_origin_y_ = nullptr;
    QPushButton     *btn_kf_width_ = nullptr;
    QPushButton     *btn_kf_height_ = nullptr;
    QPushButton     *btn_kf_text_color_ = nullptr;
    QPushButton     *btn_kf_fill_color_ = nullptr;
};
