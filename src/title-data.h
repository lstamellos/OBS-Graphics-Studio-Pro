/*
 * title-data.h
 *
 * Core data model for the OBS Titler Pro plugin.
 *
 * A Title is composed of one or more Layers. Each layer has a set of
 * Properties (position, scale, opacity, colour, text …). Properties
 * can be animated over time via Keyframes that live on a Timeline.
 *
 * The TitleDataStore is a singleton that owns all titles and persists
 * them to a JSON file in the OBS profile directory.
 */

#pragma once

#include <string>
#include <vector>
#include <map>
#include <memory>
#include <functional>
#include <cstdint>
#include <atomic>
#include <obs-module.h>
#include <util/config-file.h>

/* ══════════════════════════════════════════════════════════════════
 *  Easing / interpolation
 * ══════════════════════════════════════════════════════════════════ */
enum class EasingType {
    Linear,
    EaseIn,
    EaseOut,
    EaseInOut,
    Bezier,     /* uses cx1/cy1/cx2/cy2 control points */
    Hold,       /* no interpolation – jump cut */
};

/* ══════════════════════════════════════════════════════════════════
 *  Keyframe
 * ══════════════════════════════════════════════════════════════════ */
struct Keyframe {
    double   time   = 0.0;   /* seconds from clip start */
    double   value  = 0.0;

    EasingType easing = EasingType::EaseInOut;

    /* Bezier control points (normalised 0-1 both axes) */
    float cx1 = 0.333f, cy1 = 0.0f;
    float cx2 = 0.667f, cy2 = 1.0f;
};

/* ══════════════════════════════════════════════════════════════════
 *  Animated property – holds a list of keyframes for one numeric
 *  channel (e.g. posX, opacity …).  If no keyframes exist the
 *  static_value is used.
 * ══════════════════════════════════════════════════════════════════ */
struct AnimatedProperty {
    std::string name;
    double      static_value = 0.0;
    std::vector<Keyframe> keyframes;   /* sorted by time */

    bool is_animated() const { return !keyframes.empty(); }

    /* Evaluate the property at time t (seconds). */
    double evaluate(double t) const;

private:
    static double ease(double x, EasingType e,
                       float cx1, float cy1, float cx2, float cy2);
    static double bezierY(double t,
                          float cy1, float cy2);
};

/* ══════════════════════════════════════════════════════════════════
 *  Layer type
 * ══════════════════════════════════════════════════════════════════ */
enum class LayerType {
    Text,
    SolidRect,
    Image,
    Shape,      /* future: polygon / ellipse */
};

/* ══════════════════════════════════════════════════════════════════
 *  Layer
 * ══════════════════════════════════════════════════════════════════ */
struct Layer {
    std::string id;          /* UUID */
    std::string name;
    LayerType   type = LayerType::Text;
    bool        visible  = true;
    bool        locked   = false;
    bool        properties_expanded = false;
    std::string parent_id;

    /* Timeline in/out (seconds) within parent title clip */
    double      in_time  = 0.0;
    double      out_time = 5.0;

    /* ----- Animated properties ----- */
    AnimatedProperty pos_x   { "pos_x",    0.0 };
    AnimatedProperty pos_y   { "pos_y",    0.0 };
    AnimatedProperty scale_x { "scale_x",  1.0 };
    AnimatedProperty scale_y { "scale_y",  1.0 };
    AnimatedProperty rotation{ "rotation", 0.0 };
    AnimatedProperty opacity { "opacity",  1.0 };

    /* ----- Text-specific ----- */
    std::string text_content  = "Title";
    bool        expose_text    = false;
    std::string font_family   = "Helvetica Neue";
    int         font_size     = 72;
    bool        font_bold     = false;
    bool        font_italic   = false;
    uint32_t    text_color    = 0xFFFFFFFF;  /* ARGB */
    uint32_t    stroke_color  = 0x00000000;
    float       stroke_width  = 0.0f;
    int         align_h       = 1;  /* 0=left 1=center 2=right */
    int         align_v       = 1;  /* 0=top  1=middle 2=bottom */

    /* ----- Solid / shape ----- */
    uint32_t    fill_color    = 0xFF222222;
    float       rect_width    = 1920.0f;
    float       rect_height   = 100.0f;
    float       corner_radius = 0.0f;

    /* Keyframable geometry mirrors the static fields above so older saved
     * titles remain readable while new titles can animate size/origin.
     */
    AnimatedProperty box_width  { "box_width",  1920.0 };
    AnimatedProperty box_height { "box_height", 100.0 };

    /* ----- Geometry anchor / origin -----
     * Normalized inside the editable bounding box: 0.0 = left/top,
     * 0.5 = center, 1.0 = right/bottom. The layer position is this origin.
     */
    float       origin_x      = 0.5f;
    float       origin_y      = 0.5f;
    AnimatedProperty origin_x_prop { "origin_x", 0.5 };
    AnimatedProperty origin_y_prop { "origin_y", 0.5 };

    /* ----- Drop shadow ----- */
    bool        shadow_enabled = false;
    uint32_t    shadow_color   = 0x99000000;
    float       shadow_opacity = 0.6f;
    float       shadow_distance = 8.0f;
    float       shadow_angle = 135.0f;
    float       shadow_blur = 4.0f;
    float       shadow_spread = 0.0f;
    AnimatedProperty shadow_enabled_prop { "shadow_enabled", 0.0 };
    AnimatedProperty shadow_opacity_prop { "shadow_opacity", 0.6 };
    AnimatedProperty shadow_distance_prop { "shadow_distance", 8.0 };
    AnimatedProperty shadow_angle_prop { "shadow_angle", 135.0 };
    AnimatedProperty shadow_blur_prop { "shadow_blur", 4.0 };
    AnimatedProperty shadow_spread_prop { "shadow_spread", 0.0 };
    AnimatedProperty shadow_color_a { "shadow_color_a", 153.0 };
    AnimatedProperty shadow_color_r { "shadow_color_r", 0.0 };
    AnimatedProperty shadow_color_g { "shadow_color_g", 0.0 };
    AnimatedProperty shadow_color_b { "shadow_color_b", 0.0 };

    /* ----- Keyframable color channels, 0-255 ARGB. */
    AnimatedProperty text_color_a { "text_color_a", 255.0 };
    AnimatedProperty text_color_r { "text_color_r", 255.0 };
    AnimatedProperty text_color_g { "text_color_g", 255.0 };
    AnimatedProperty text_color_b { "text_color_b", 255.0 };
    AnimatedProperty fill_color_a { "fill_color_a", 255.0 };
    AnimatedProperty fill_color_r { "fill_color_r",  34.0 };
    AnimatedProperty fill_color_g { "fill_color_g",  34.0 };
    AnimatedProperty fill_color_b { "fill_color_b",  34.0 };

    /* ----- Image ----- */
    std::string image_path;
    bool        lock_aspect_ratio = true;
};

/* ══════════════════════════════════════════════════════════════════
 *  Title
 * ══════════════════════════════════════════════════════════════════ */
struct Title {
    std::string id;
    std::string name        = "Untitled";
    double      duration    = 5.0;   /* total clip duration (seconds) */
    double      loop_start  = 1.0;   /* live-cue loop start (seconds) */
    double      loop_end    = 4.0;   /* live-cue loop end (seconds) */
    int         playback_mode = 0;   /* 0=play once, 1=loop in/out, 2=pause at position */
    int         loop_type     = 0;   /* 0=restart, 1=ping-pong */
    double      pause_time    = 0.0; /* seconds from timeline start */
    uint32_t    bg_color    = 0x00000000;  /* transparent by default */
    int         width       = 1920;
    int         height      = 1080;

    std::vector<std::shared_ptr<Layer>> layers;  /* bottom → top order */
    std::vector<std::vector<std::string>> live_text_rows;
    int current_cue_row = -1; /* runtime-only active live text row */
    int pending_cue_row = -1; /* runtime-only next row waiting for outro */
    uint64_t cue_revision = 0; /* runtime-only live text cue counter */

    /* Helpers */
    std::shared_ptr<Layer> find_layer(const std::string &layer_id) const;
    void add_layer(std::shared_ptr<Layer> l);
    void remove_layer(const std::string &layer_id);
    void move_layer(const std::string &layer_id, int delta);
};

/* ══════════════════════════════════════════════════════════════════
 *  TitleDataStore  (singleton)
 * ══════════════════════════════════════════════════════════════════ */
class TitleDataStore {
public:
    static TitleDataStore &instance();
    static std::string make_uuid();

    /* CRUD */
    std::shared_ptr<Title> create_title(const std::string &name = "New Title");
    std::shared_ptr<Title> get_title(const std::string &id) const;
    void                   delete_title(const std::string &id);
    void                   rename_title(const std::string &id,
                                        const std::string &name);
    bool                   export_title(const std::string &id,
                                        const std::string &path,
                                        std::string *error = nullptr) const;
    std::shared_ptr<Title> import_title(const std::string &path,
                                        std::string *error = nullptr);

    const std::vector<std::shared_ptr<Title>> &titles() const { return titles_; }

    /* Persistence */
    void load();
    void save() const;

    /* Change notifications */
    using ChangeCallback = std::function<void()>;
    void on_change(ChangeCallback cb) { change_cbs_.push_back(cb); }
    void notify_change();
    void touch_runtime_change();
    uint64_t revision() const { return revision_.load(); }

private:
    TitleDataStore() = default;
    std::vector<std::shared_ptr<Title>>  titles_;
    std::vector<ChangeCallback>          change_cbs_;
    std::atomic<uint64_t>                revision_ { 0 };

    static std::string data_path();
};
