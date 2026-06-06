/*
 * title-source.cpp
 *
 * OBS source: renders a Title through the OBS-native GPU pipeline.
 *
 * CPU 2-D raster backends are intentionally absent from the live OBS render
 * path; drawing, transforms, blending, and effects are delegated to
 * title-renderer-gpu.* and libobs gs_* passes.
 */

#include "title-source.h"
#include "title-renderer-gpu.h"
#include "title-data.h"
#include "plugin-main.h"
#include "title-localization.h"

#include <obs-module.h>
#include <graphics/graphics.h>
#include <util/threading.h>

#include <QImage>
#include <QString>
#include <QDateTime>

#include <memory>
#include <string>
#include <cstring>
#include <cmath>
#include <chrono>
#include <vector>
#include <algorithm>
#include <mutex>
#include <limits>

namespace {
constexpr uint32_t kMaxSourceDimension = 16384;

static uint32_t clamped_source_dimension(int value)
{
    return static_cast<uint32_t>(std::clamp(value, 1, static_cast<int>(kMaxSourceDimension)));
}

}

/* ══════════════════════════════════════════════════════════════════
 *  Source private data
 * ══════════════════════════════════════════════════════════════════ */
struct TitleSourceData {
    obs_source_t *source  = nullptr;

    /* Settings */
    std::string title_id;
    bool        loop         = true;
    float       speed        = 1.0f;
    bool        auto_advance = false;  /* future: playlist mode */

    enum class CuePhase { FreeRun, IntroLoop, OutroThenIntro, OutroOnly };

    /* Playback state */
    double      playhead     = 0.0;    /* seconds */
    bool        playing      = true;
    bool        playback_reverse = false;
    uint64_t    seen_cue_revision = 0;
    CuePhase    cue_phase    = CuePhase::FreeRun;
    int         active_cue_row = -1;
    std::chrono::steady_clock::time_point last_tick;
    std::chrono::steady_clock::time_point last_clock_refresh;
    bool        first_tick   = true;
    bool        waiting_for_cue = true;

    /* GPU pipeline target */
    std::mutex    render_mutex;
    obsgs::ObsGpuRenderPipeline gpu_pipeline;
    obsgs::GpuTitlePlan gpu_plan;

    /* Dirty flag – avoid re-uploading unchanged frames */
    bool dirty = true;
    uint64_t seen_store_revision = 0;
};


static bool layer_has_animation(const Layer &layer)
{
    return layer.pos_x.is_animated() ||
           layer.pos_y.is_animated() ||
           layer.scale_x.is_animated() ||
           layer.scale_y.is_animated() ||
           layer.rotation.is_animated() ||
           layer.opacity.is_animated() ||
           layer.box_width.is_animated() ||
           layer.box_height.is_animated() ||
           layer.origin_x_prop.is_animated() ||
           layer.origin_y_prop.is_animated() ||
           layer.shadow_enabled_prop.is_animated() ||
           layer.shadow_opacity_prop.is_animated() ||
           layer.shadow_distance_prop.is_animated() ||
           layer.shadow_angle_prop.is_animated() ||
           layer.shadow_blur_prop.is_animated() ||
           layer.shadow_spread_prop.is_animated() ||
           layer.shadow_color_a.is_animated() ||
           layer.shadow_color_r.is_animated() ||
           layer.shadow_color_g.is_animated() ||
           layer.shadow_color_b.is_animated() ||
           layer.background_enabled_prop.is_animated() ||
           layer.background_opacity_prop.is_animated() ||
           layer.background_padding_x_prop.is_animated() ||
           layer.background_padding_y_prop.is_animated() ||
           layer.background_corner_radius_prop.is_animated() ||
           layer.background_color_a.is_animated() ||
           layer.background_color_r.is_animated() ||
           layer.background_color_g.is_animated() ||
           layer.background_color_b.is_animated() ||
           layer.text_color_a.is_animated() ||
           layer.text_color_r.is_animated() ||
           layer.text_color_g.is_animated() ||
           layer.text_color_b.is_animated() ||
           layer.fill_color_a.is_animated() ||
           layer.fill_color_r.is_animated() ||
           layer.fill_color_g.is_animated() ||
           layer.fill_color_b.is_animated();
}

static bool include_property_bounds(const Layer &layer, const AnimatedProperty &prop,
                                    double &first_time, double &last_time)
{
    if (prop.keyframes.empty()) return false;
    first_time = std::min(first_time, layer.in_time + prop.keyframes.front().time);
    last_time = std::max(last_time, layer.in_time + prop.keyframes.back().time);
    return true;
}

static bool layer_animation_keyframe_bounds(const Layer &layer, double &first_time, double &last_time)
{
    bool has_bounds = false;
    has_bounds |= include_property_bounds(layer, layer.pos_x, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.pos_y, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.scale_x, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.scale_y, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.rotation, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.opacity, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.box_width, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.box_height, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.origin_x_prop, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.origin_y_prop, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.shadow_enabled_prop, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.shadow_opacity_prop, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.shadow_distance_prop, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.shadow_angle_prop, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.shadow_blur_prop, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.shadow_spread_prop, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.shadow_color_a, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.shadow_color_r, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.shadow_color_g, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.shadow_color_b, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.background_enabled_prop, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.background_opacity_prop, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.background_padding_x_prop, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.background_padding_y_prop, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.background_corner_radius_prop, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.background_color_a, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.background_color_r, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.background_color_g, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.background_color_b, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.text_color_a, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.text_color_r, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.text_color_g, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.text_color_b, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.fill_color_a, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.fill_color_r, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.fill_color_g, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.fill_color_b, first_time, last_time);
    return has_bounds;
}

static bool title_has_clock_layer(const std::shared_ptr<Title> &title)
{
    if (!title) return false;
    return std::any_of(title->layers.begin(), title->layers.end(),
                       [](const std::shared_ptr<Layer> &layer) {
                           return layer && layer->type == LayerType::Clock;
                       });
}

static bool title_has_ticker_layer(const std::shared_ptr<Title> &title)
{
    if (!title) return false;
    return std::any_of(title->layers.begin(), title->layers.end(),
                       [](const std::shared_ptr<Layer> &layer) {
                           return layer && layer->type == LayerType::Ticker;
                       });
}

static bool title_has_animation(const std::shared_ptr<Title> &title)
{
    if (!title) return false;
    return std::any_of(title->layers.begin(), title->layers.end(),
                       [](const std::shared_ptr<Layer> &layer) {
                           return layer && layer_has_animation(*layer);
                       });
}

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


static std::vector<std::shared_ptr<Layer>> exposed_text_layers(const Title &title)
{
    std::vector<std::shared_ptr<Layer>> exposed;
    for (const auto &layer : title.layers) {
        if (!layer) continue;
        if ((layer->type == LayerType::Text || layer->type == LayerType::Ticker) && layer->expose_text)
            exposed.push_back(layer);
    }
    return order_exposed_text_layers(exposed, title.live_text_column_order);
}

static double cue_persistence_hold_time(const Title &title)
{
    if (title.playback_mode == 1)
        return std::clamp(title.loop_end, title.loop_start, title.duration);
    if (title.playback_mode == 2)
        return std::clamp(title.pause_time, 0.0, title.duration);
    return std::clamp(title.duration, 0.0, title.duration);
}

static void clear_cue_persistence_transition(const std::shared_ptr<Title> &title)
{
    if (!title || !title->cue_persistence_transition) return;
    title->cue_persistence_transition = false;
    title->cue_persistent_text_columns.clear();
    TitleDataStore::instance().touch_runtime_change();
}

static int exposed_text_layer_index(const std::vector<std::shared_ptr<Layer>> &exposed, const std::shared_ptr<Layer> &layer)
{
    for (int i = 0; i < (int)exposed.size(); ++i) {
        if (exposed[i] == layer)
            return i;
    }
    return -1;
}



static void apply_live_text_row(const std::shared_ptr<Title> &title, int row)
{
    if (!title || row < 0 || row >= (int)title->live_text_rows.size()) return;
    auto exposed = exposed_text_layers(title);
    for (int col = 0; col < (int)exposed.size() && col < (int)title->live_text_rows[row].size(); ++col)
        exposed[col]->text_content = title->live_text_rows[row][col];
}

QImage render_title_to_image(const Title &, double)
{
    /* Snapshot export is intentionally disabled until the GPU renderer grows an
     * OBS render-target readback path.  Returning a null image prevents legacy
     * CPU raster backends from being used for screenshots.
     */
    return QImage();
}

/* ══════════════════════════════════════════════════════════════════
 *  OBS source callbacks
 * ══════════════════════════════════════════════════════════════════ */
static const char *source_get_name(void *)
{
    return obsgs_tr_c("OBSTitles.SourceName");
}

static void *source_create(obs_data_t *settings, obs_source_t *source)
{
    auto *data = new TitleSourceData();
    data->source    = source;
    data->title_id  = obs_data_get_string(settings, PROP_TITLE_ID);
    data->loop      = obs_data_get_bool(settings,   PROP_LOOP);
    data->speed     = (float)obs_data_get_double(settings, PROP_SPEED);
    data->last_tick = std::chrono::steady_clock::now();
    data->last_clock_refresh = data->last_tick;
    if (auto title = TitleDataStore::instance().get_title(data->title_id))
        data->seen_cue_revision = title->cue_revision;
    data->playing = false;
    data->waiting_for_cue = true;
    data->active_cue_row = -1;
    data->dirty = true;
    return data;
}

static void source_destroy(void *priv)
{
    auto *data = static_cast<TitleSourceData *>(priv);
    {
        std::lock_guard<std::mutex> lock(data->render_mutex);
        data->gpu_pipeline.reset();
    }
    delete data;
}

static void source_update(void *priv, obs_data_t *settings)
{
    auto *data = static_cast<TitleSourceData *>(priv);
    data->title_id = obs_data_get_string(settings, PROP_TITLE_ID);
    data->loop     = obs_data_get_bool(settings,   PROP_LOOP);
    data->speed    = (float)obs_data_get_double(settings, PROP_SPEED);
    data->playhead = 0.0;
    data->playback_reverse = false;
    data->cue_phase = TitleSourceData::CuePhase::FreeRun;
    data->playing = false;
    data->waiting_for_cue = true;
    data->active_cue_row = -1;
    if (auto title = TitleDataStore::instance().get_title(data->title_id))
        data->seen_cue_revision = title->cue_revision;
    else
        data->seen_cue_revision = 0;
    data->last_clock_refresh = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(data->render_mutex);
        data->gpu_pipeline.reset();
    }
    data->dirty    = true;
}

static uint32_t source_get_width(void *priv)
{
    auto *data = static_cast<TitleSourceData *>(priv);
    auto title = TitleDataStore::instance().get_title(data->title_id);
    return title ? clamped_source_dimension(title->width) : 1920;
}

static uint32_t source_get_height(void *priv)
{
    auto *data = static_cast<TitleSourceData *>(priv);
    auto title = TitleDataStore::instance().get_title(data->title_id);
    return title ? clamped_source_dimension(title->height) : 1080;
}

static void source_video_tick(void *priv, float seconds)
{
    auto *data = static_cast<TitleSourceData *>(priv);
    if (data->title_id.empty()) return;

    auto title = TitleDataStore::instance().get_title(data->title_id);
    if (!title) return;

    if (title->cue_revision != data->seen_cue_revision) {
        double loop_end = std::clamp(title->loop_end, title->loop_start, title->duration);
        double pause_time = std::clamp(title->pause_time, 0.0, title->duration);
        bool has_pending = title->pending_cue_row >= 0 &&
                           title->pending_cue_row < (int)title->live_text_rows.size();
        bool has_current = title->current_cue_row >= 0 &&
                           title->current_cue_row < (int)title->live_text_rows.size();
        bool is_uncue = !has_pending && !has_current && data->active_cue_row >= 0;
        if (title->playback_mode == 1) {
            if (has_pending) {
                data->playhead = loop_end;
                data->cue_phase = TitleSourceData::CuePhase::OutroThenIntro;
            } else if (is_uncue) {
                data->playhead = loop_end;
                data->cue_phase = TitleSourceData::CuePhase::OutroOnly;
            } else {
                data->playhead = 0.0;
                data->cue_phase = TitleSourceData::CuePhase::IntroLoop;
            }
        } else if (title->playback_mode == 2 && (has_pending || is_uncue)) {
            data->playhead = pause_time;
            data->cue_phase = has_pending
                ? TitleSourceData::CuePhase::OutroThenIntro
                : TitleSourceData::CuePhase::OutroOnly;
        } else {
            if (has_pending) {
                apply_live_text_row(title, title->pending_cue_row);
                title->current_cue_row = title->pending_cue_row;
                title->pending_cue_row = -1;
                has_current = true;
                TitleDataStore::instance().touch_runtime_change();
            }
            if (!is_uncue)
                data->playhead = 0.0;
            data->cue_phase = TitleSourceData::CuePhase::FreeRun;
        }
        if (has_current)
            data->active_cue_row = title->current_cue_row;
        data->seen_cue_revision = title->cue_revision;
        data->playback_reverse = false;
        data->waiting_for_cue = false;
        data->playing = true;
        data->dirty = true;
    }

    const bool has_clock_layer = title_has_clock_layer(title);
    const bool has_ticker_layer = title_has_ticker_layer(title);
    const bool has_timeline_animation = title_has_animation(title);
    const bool static_clock_title = has_clock_layer && !has_timeline_animation;

    if (data->playing && !static_clock_title) {
        double dt = (double)seconds * data->speed;
        double duration = std::max(0.001, title->duration);
        double loop_start = std::clamp(title->loop_start, 0.0, title->duration);
        double loop_end = std::clamp(title->loop_end, loop_start, title->duration);

        const bool ping_pong_loop = title->playback_mode == 1 && title->loop_type == 1 &&
                                    (data->cue_phase == TitleSourceData::CuePhase::FreeRun ||
                                     data->cue_phase == TitleSourceData::CuePhase::IntroLoop);
        if (ping_pong_loop) {
            data->playhead += data->playback_reverse ? -dt : dt;
        } else {
            data->playhead += dt;
        }

        if (data->cue_phase == TitleSourceData::CuePhase::IntroLoop && loop_end > loop_start) {
            double loop_len = std::max(0.001, loop_end - loop_start);
            if (title->loop_type == 1) {
                if (!data->playback_reverse && data->playhead >= loop_end) {
                    clear_cue_persistence_transition(title);
                    data->playhead = loop_end - std::fmod(data->playhead - loop_end, loop_len);
                    data->playback_reverse = true;
                } else if (data->playback_reverse && data->playhead <= loop_start) {
                    data->playhead = loop_start + std::fmod(loop_start - data->playhead, loop_len);
                    data->playback_reverse = false;
                }
            } else if (data->playhead >= loop_end) {
                clear_cue_persistence_transition(title);
                data->playhead = loop_start + std::fmod(data->playhead - loop_start, loop_len);
            }
        } else if ((data->cue_phase == TitleSourceData::CuePhase::OutroThenIntro ||
                    data->cue_phase == TitleSourceData::CuePhase::OutroOnly) &&
                   data->playhead >= title->duration) {
            double next_intro_time = std::max(0.0, data->playhead - title->duration);
            if (data->cue_phase == TitleSourceData::CuePhase::OutroOnly) {
                data->playhead = title->duration;
                data->playing = false;
                data->cue_phase = TitleSourceData::CuePhase::FreeRun;
                data->active_cue_row = -1;
                title->current_cue_row = -1;
                title->pending_cue_row = -1;
                title->cue_persistence_transition = false;
                title->cue_persistent_text_columns.clear();
                TitleDataStore::instance().touch_runtime_change();
            } else {
                if (title->pending_cue_row >= 0 && title->pending_cue_row < (int)title->live_text_rows.size()) {
                    apply_live_text_row(title, title->pending_cue_row);
                    title->current_cue_row = title->pending_cue_row;
                    title->pending_cue_row = -1;
                    data->active_cue_row = title->current_cue_row;
                    TitleDataStore::instance().touch_runtime_change();
                }
                if (title->playback_mode == 1) {
                    if (loop_end > loop_start && next_intro_time >= loop_end) {
                        next_intro_time = loop_start + std::fmod(next_intro_time - loop_start,
                                                                 std::max(0.001, loop_end - loop_start));
                    }
                    data->playhead = std::clamp(next_intro_time, 0.0, title->duration);
                    data->cue_phase = TitleSourceData::CuePhase::IntroLoop;
                } else {
                    data->playhead = 0.0;
                    data->cue_phase = TitleSourceData::CuePhase::FreeRun;
                }
            }
            data->playback_reverse = false;
        } else if (data->cue_phase == TitleSourceData::CuePhase::FreeRun) {
            if (title->playback_mode == 1) {
                double loop_len = std::max(0.001, loop_end - loop_start);
                if (loop_end <= loop_start + 0.0001) {
                    if (data->playhead >= title->duration)
                        data->playhead = std::fmod(data->playhead, duration);
                } else if (title->loop_type == 1) {
                    if (!data->playback_reverse && data->playhead >= loop_end) {
                        data->playhead = loop_end - std::fmod(data->playhead - loop_end, loop_len);
                        data->playback_reverse = true;
                    } else if (data->playback_reverse && data->playhead <= loop_start) {
                        data->playhead = loop_start + std::fmod(loop_start - data->playhead, loop_len);
                        data->playback_reverse = false;
                    }
                } else if (data->playhead >= loop_end) {
                    data->playhead = loop_start + std::fmod(data->playhead - loop_end, loop_len);
                }
            } else if (title->playback_mode == 2) {
                double pause_time = std::clamp(title->pause_time, 0.0, title->duration);
                if (data->playhead >= pause_time) {
                    data->playhead = pause_time;
                    data->playing = false;
                    clear_cue_persistence_transition(title);
                }
            } else if (data->playhead >= title->duration) {
                data->playhead = title->duration;
                data->playing  = false;
                if (title->current_cue_row >= 0 || title->pending_cue_row >= 0 || data->active_cue_row >= 0) {
                    title->current_cue_row = -1;
                    title->pending_cue_row = -1;
                    title->cue_persistence_transition = false;
                    title->cue_persistent_text_columns.clear();
                    data->active_cue_row = -1;
                    TitleDataStore::instance().touch_runtime_change();
                }
            }
        }
        data->dirty = true;
    }


    if (has_ticker_layer)
        data->dirty = true;

    if (static_clock_title || (!data->playing && has_clock_layer)) {
        auto now = std::chrono::steady_clock::now();
        if (now - data->last_clock_refresh >= std::chrono::seconds(1)) {
            data->last_clock_refresh = now;
            data->dirty = true;
        }
    }

    uint64_t revision = TitleDataStore::instance().revision();
    if (revision != data->seen_store_revision) {
        data->seen_store_revision = revision;
        data->dirty = true;
    }

    if (data->dirty) {
        data->gpu_plan = data->gpu_pipeline.build_migration_plan(*title);
        data->dirty = false;
    }
}

static void source_video_render(void *priv, gs_effect_t * /*effect*/)
{
    auto *data = static_cast<TitleSourceData *>(priv);
    if (data->title_id.empty()) return;

    auto title = TitleDataStore::instance().get_title(data->title_id);
    if (!title) return;

    std::lock_guard<std::mutex> lock(data->render_mutex);
    data->gpu_pipeline.render_title(*title, data->playhead);
}

/* ── Properties panel ─────────────────────────────────────────────── */
static obs_properties_t *source_get_properties(void * /*priv*/)
{
    obs_properties_t *props = obs_properties_create();

    /* Title selector */
    obs_property_t *p = obs_properties_add_list(
        props, PROP_TITLE_ID, obsgs_tr_c("OBSTitles.TitleID"),
        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);

    obs_property_list_add_string(p, obsgs_tr_c("OBSTitles.NoTitle"), "");
    for (auto &t : TitleDataStore::instance().titles())
        obs_property_list_add_string(p, t->name.c_str(), t->id.c_str());

    obs_properties_add_bool(props,   PROP_LOOP,  obsgs_tr_c("OBSTitles.Loop"));
    obs_properties_add_float_slider(props, PROP_SPEED,
        obsgs_tr_c("OBSTitles.Speed"), 0.1, 4.0, 0.05);

    return props;
}

static void source_get_defaults(obs_data_t *settings)
{
    obs_data_set_default_string(settings, PROP_TITLE_ID, "");
    obs_data_set_default_bool(settings,   PROP_LOOP,     true);
    obs_data_set_default_double(settings, PROP_SPEED,    1.0);
}

/* ══════════════════════════════════════════════════════════════════
 *  Registration
 * ══════════════════════════════════════════════════════════════════ */
void title_source_register()
{
    static obs_source_info si = {};
    si.id             = "obs_graphics_studio_pro_source";
    si.type           = OBS_SOURCE_TYPE_INPUT;
    si.output_flags   = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW;
    si.get_name       = source_get_name;
    si.create         = source_create;
    si.destroy        = source_destroy;
    si.update         = source_update;
    si.get_width      = source_get_width;
    si.get_height     = source_get_height;
    si.video_tick     = source_video_tick;
    si.video_render   = source_video_render;
    si.get_properties = source_get_properties;
    si.get_defaults   = source_get_defaults;

    obs_register_source(&si);
    blog(LOG_INFO, "[OBS Graphics Studio Pro] Source type registered.");
}
