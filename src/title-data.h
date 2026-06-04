/*
 * title-data.h
 *
 * Core data model for the OBS Graphics Studio Pro plugin.
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
#include <mutex>
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
    static double bezierY(double x,
                          float cx1, float cy1,
                          float cx2, float cy2);
};

/* ══════════════════════════════════════════════════════════════════
 *  Layer type
 * ══════════════════════════════════════════════════════════════════ */
enum class LayerType {
    Text,
    SolidRect,
    Image,
    Shape,      /* future: polygon / ellipse */
    Clock,
    Ticker,
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
    std::string clock_format  = "H:i:s";  /* PHP date()-style format for clock layers */
    bool        expose_text    = false;
    std::string font_family   = "Helvetica Neue";
    std::string font_style    = "Regular";
    int         font_size     = 72;
    bool        font_bold     = false;
    bool        font_italic   = false;
    bool        font_kerning  = true;
    int         kerning_mode  = 0;  /* 0=metrics, 1=optical, 2=manual */
    float       manual_kerning = 0.0f;
    float       text_leading  = 0.0f;
    float       char_tracking = 0.0f;
    float       char_scale_x  = 1.0f;
    float       char_scale_y  = 1.0f;
    float       baseline_shift = 0.0f;
    int         text_style    = 0;  /* 0=normal, 1=all caps, 2=small caps, 3=superscript, 4=subscript */
    bool        text_underline = false;
    bool        text_strikethrough = false;
    bool        text_ligatures = true;
    bool        text_stylistic_alternates = false;
    bool        text_fractions = false;
    bool        text_opentype_features = false;
    std::string text_language = "English";
    int         text_overflow_mode = 0;  /* 0=wrap, 1=clip, 2=horizontal fit */
    float       text_fit_min_scale = 0.5f;

    /* ----- Ticker-specific -----
     * style: 0=horizontal scrolling, 1=vertical line-by-line, 2=vertical smooth.
     * direction: horizontal 0=left-to-right, 1=right-to-left; vertical 0=top-to-bottom, 1=bottom-to-top.
     * speed is pixels/second. line_hold is seconds between line-by-line moves.
     */
    int         ticker_style = 0;
    double      ticker_speed = 120.0;
    double      ticker_line_hold = 2.0;
    int         ticker_direction = 1;

    uint32_t    text_color    = 0xFFFFFFFF;  /* ARGB */

    /* ----- Outline shared by text and solid/shape layers ----- */
    bool        outline_enabled = false;
    uint32_t    stroke_color  = 0xFF000000;
    float       stroke_width  = 0.0f;
    float       outline_opacity = 1.0f;
    int         outline_join_style = 1;  /* 0=miter, 1=round, 2=bevel */
    bool        outline_on_front = true;
    bool        outline_antialias = true;

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

struct TitleTemplateExportMetadata {
    std::string title;
    std::string description;
    std::string creator;
    std::string creation_date;
    std::string screenshot_png_base64;
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
    bool                   export_title(const std::string &id,
                                        const std::string &path,
                                        const TitleTemplateExportMetadata &metadata,
                                        std::string *error = nullptr) const;
    std::shared_ptr<Title> import_title(const std::string &path,
                                        std::string *error = nullptr);

    std::vector<std::shared_ptr<Title>> titles() const;

    /* Persistence */
    void load();
    void save() const;

    /* Change notifications */
    using ChangeCallback = std::function<void()>;
    void on_change(ChangeCallback cb);
    void notify_change();
    void touch_runtime_change();
    uint64_t revision() const { return revision_.load(); }

private:
    TitleDataStore() = default;
    mutable std::recursive_mutex         mutex_;
    std::vector<std::shared_ptr<Title>>  titles_;
    std::vector<ChangeCallback>          change_cbs_;
    std::atomic<uint64_t>                revision_ { 0 };

    static std::string data_path();
};
