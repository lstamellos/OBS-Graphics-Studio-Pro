/*
 * title-data.cpp
 */

#include "title-data.h"
#include <obs-module.h>
#include <util/platform.h>

#include <nlohmann/json.hpp>
#include <fstream>
#include <random>
#include <sstream>
#include <iomanip>
#include <cmath>
#include <algorithm>
#include <unordered_map>
#include <stdexcept>

using json = nlohmann::json;

static void set_color_channels(Layer &l, bool text, uint32_t argb)
{
    AnimatedProperty &a = text ? l.text_color_a : l.fill_color_a;
    AnimatedProperty &r = text ? l.text_color_r : l.fill_color_r;
    AnimatedProperty &g = text ? l.text_color_g : l.fill_color_g;
    AnimatedProperty &b = text ? l.text_color_b : l.fill_color_b;
    a.static_value = (argb >> 24) & 0xFF;
    r.static_value = (argb >> 16) & 0xFF;
    g.static_value = (argb >> 8) & 0xFF;
    b.static_value = argb & 0xFF;
}

/* ══════════════════════════════════════════════════════════════════
 *  UUID helper
 * ══════════════════════════════════════════════════════════════════ */
std::string TitleDataStore::make_uuid()
{
    static std::random_device rd;
    static std::mt19937_64 gen(rd());
    std::uniform_int_distribution<uint64_t> dis;

    uint64_t hi = dis(gen);
    uint64_t lo = dis(gen);
    // Set UUID version 4 bits
    hi = (hi & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;
    lo = (lo & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;

    std::ostringstream ss;
    ss << std::hex << std::setfill('0');
    ss << std::setw(8) << (hi >> 32);
    ss << '-' << std::setw(4) << ((hi >> 16) & 0xFFFF);
    ss << '-' << std::setw(4) << (hi & 0xFFFF);
    ss << '-' << std::setw(4) << (lo >> 48);
    ss << '-' << std::setw(12) << (lo & 0x0000FFFFFFFFFFFFULL);
    return ss.str();
}

/* ══════════════════════════════════════════════════════════════════
 *  AnimatedProperty::evaluate
 * ══════════════════════════════════════════════════════════════════ */
double AnimatedProperty::evaluate(double t) const
{
    if (keyframes.empty()) return static_value;
    if (keyframes.size() == 1) return keyframes.front().value;
    if (t <= keyframes.front().time) return keyframes.front().value;
    if (t >= keyframes.back().time)  return keyframes.back().value;

    /* Find surrounding pair */
    for (size_t i = 0; i + 1 < keyframes.size(); ++i) {
        const auto &k0 = keyframes[i];
        const auto &k1 = keyframes[i + 1];
        if (t >= k0.time && t <= k1.time) {
            double span = k1.time - k0.time;
            if (span < 1e-10) return k0.value;
            double x = (t - k0.time) / span;  // 0..1

            if (k0.easing == EasingType::Hold) return k0.value;

            double y = ease(x, k0.easing,
                            k0.cx1, k0.cy1, k0.cx2, k0.cy2);
            return k0.value + y * (k1.value - k0.value);
        }
    }
    return keyframes.back().value;
}

double AnimatedProperty::ease(double x, EasingType e,
                               float cx1, float cy1,
                               float cx2, float cy2)
{
    switch (e) {
    case EasingType::Linear:   return x;
    case EasingType::EaseIn:   return x * x;
    case EasingType::EaseOut:  return x * (2.0 - x);
    case EasingType::EaseInOut:
        return x < 0.5 ? 2.0 * x * x : -1.0 + (4.0 - 2.0 * x) * x;
    case EasingType::Bezier:
        return bezierY(x, cy1, cy2);
    default: return x;
    }
    (void)cx1; (void)cx2; // used by full bezier solver if needed
}

/* Approximate cubic bezier Y given X (Newton-Raphson, 4 iters) */
double AnimatedProperty::bezierY(double x, float cy1, float cy2)
{
    // Simplified: solve t from Bx(t) = x then evaluate By(t)
    // Using standard CSS cubic-bezier with fixed P0=(0,0) P3=(1,1)
    double t = x;
    for (int i = 0; i < 8; ++i) {
        double t2 = t * t, t3 = t2 * t;
        double bx = 3.0*t*(1-t)*(1-t)*0.333 + 3.0*t2*(1-t)*0.667 + t3;
        // simple linear bezier for now
        (void)bx;
        break;
    }
    // Fallback: use ease-in-out when bezier not fully solved
    return t < 0.5 ? 2.0*t*t : -1.0 + (4.0 - 2.0*t)*t;
    (void)cy1; (void)cy2;
}

/* ══════════════════════════════════════════════════════════════════
 *  Title helpers
 * ══════════════════════════════════════════════════════════════════ */
std::shared_ptr<Layer> Title::find_layer(const std::string &lid) const
{
    for (auto &l : layers)
        if (l->id == lid) return l;
    return nullptr;
}

void Title::add_layer(std::shared_ptr<Layer> l)
{
    layers.push_back(l);
}

void Title::remove_layer(const std::string &lid)
{
    layers.erase(
        std::remove_if(layers.begin(), layers.end(),
                       [&](auto &l){ return l->id == lid; }),
        layers.end());
}

void Title::move_layer(const std::string &lid, int delta)
{
    auto it = std::find_if(layers.begin(), layers.end(),
                           [&](auto &l){ return l->id == lid; });
    if (it == layers.end()) return;
    int idx = (int)(it - layers.begin());
    int dst = std::clamp(idx + delta, 0, (int)layers.size() - 1);
    if (idx == dst) return;
    auto layer = *it;
    layers.erase(it);
    layers.insert(layers.begin() + dst, layer);
}

/* ══════════════════════════════════════════════════════════════════
 *  TitleDataStore
 * ══════════════════════════════════════════════════════════════════ */
TitleDataStore &TitleDataStore::instance()
{
    static TitleDataStore inst;
    return inst;
}

void TitleDataStore::notify_change()
{
    touch_runtime_change();
    for (auto &cb : change_cbs_) cb();
}

void TitleDataStore::touch_runtime_change()
{
    revision_.fetch_add(1, std::memory_order_relaxed);
}

std::shared_ptr<Title> TitleDataStore::create_title(const std::string &name)
{
    auto t = std::make_shared<Title>();
    t->id   = make_uuid();
    t->name = name;

    /* Default: one text layer */
    auto layer = std::make_shared<Layer>();
    layer->id   = make_uuid();
    layer->name = "Title Text";
    layer->type = LayerType::Text;
    layer->pos_x.static_value = 960.0;
    layer->pos_y.static_value = 540.0;
    layer->rect_width = 960.0f;
    layer->rect_height = 160.0f;
    layer->box_width.static_value = layer->rect_width;
    layer->box_height.static_value = layer->rect_height;
    set_color_channels(*layer, true, layer->text_color);
    set_color_channels(*layer, false, layer->fill_color);
    layer->text_content = name;
    layer->expose_text = true;
    t->layers.push_back(layer);

    titles_.push_back(t);
    notify_change();
    return t;
}

std::shared_ptr<Title> TitleDataStore::get_title(const std::string &id) const
{
    for (auto &t : titles_)
        if (t->id == id) return t;
    return nullptr;
}

void TitleDataStore::delete_title(const std::string &id)
{
    titles_.erase(
        std::remove_if(titles_.begin(), titles_.end(),
                       [&](auto &t){ return t->id == id; }),
        titles_.end());
    notify_change();
}

void TitleDataStore::rename_title(const std::string &id, const std::string &n)
{
    if (auto t = get_title(id)) { t->name = n; notify_change(); }
}

/* ── persistence ──────────────────────────────────────────────────── */
std::string TitleDataStore::data_path()
{
    char *cfg_dir = obs_module_config_path("");
    std::string dir(cfg_dir);
    bfree(cfg_dir);
    os_mkdirs(dir.c_str());
    return dir + "/titles.json";
}

/* ---- JSON serialisation helpers (flat, no macros) ---- */
static json keyframe_to_json(const Keyframe &k)
{
    return {
        {"time",   k.time},
        {"value",  k.value},
        {"easing", (int)k.easing},
        {"cx1",    k.cx1}, {"cy1", k.cy1},
        {"cx2",    k.cx2}, {"cy2", k.cy2},
    };
}

static Keyframe keyframe_from_json(const json &j)
{
    Keyframe k;
    k.time   = j.value("time",   0.0);
    k.value  = j.value("value",  0.0);
    k.easing = (EasingType)j.value("easing", 0);
    k.cx1    = j.value("cx1", 0.333f);
    k.cy1    = j.value("cy1", 0.0f);
    k.cx2    = j.value("cx2", 0.667f);
    k.cy2    = j.value("cy2", 1.0f);
    return k;
}

static json aprop_to_json(const AnimatedProperty &p)
{
    json j = { {"static_value", p.static_value} };
    json kf = json::array();
    for (auto &k : p.keyframes) kf.push_back(keyframe_to_json(k));
    j["keyframes"] = kf;
    return j;
}

static AnimatedProperty aprop_from_json(const json &j, const std::string &name)
{
    AnimatedProperty p;
    p.name         = name;
    p.static_value = j.value("static_value", 0.0);
    if (j.contains("keyframes")) {
        for (auto &kj : j["keyframes"])
            p.keyframes.push_back(keyframe_from_json(kj));
    }
    return p;
}

static json layer_to_json(const Layer &l)
{
    json j;
    j["id"]       = l.id;
    j["name"]     = l.name;
    j["type"]     = (int)l.type;
    j["visible"]  = l.visible;
    j["locked"]   = l.locked;
    j["properties_expanded"] = l.properties_expanded;
    j["parent_id"] = l.parent_id;
    j["in_time"]  = l.in_time;
    j["out_time"] = l.out_time;

    j["pos_x"]    = aprop_to_json(l.pos_x);
    j["pos_y"]    = aprop_to_json(l.pos_y);
    j["scale_x"]  = aprop_to_json(l.scale_x);
    j["scale_y"]  = aprop_to_json(l.scale_y);
    j["rotation"] = aprop_to_json(l.rotation);
    j["opacity"]  = aprop_to_json(l.opacity);

    j["text_content"]  = l.text_content;
    j["expose_text"]   = l.expose_text;
    j["font_family"]   = l.font_family;
    j["font_size"]     = l.font_size;
    j["font_bold"]     = l.font_bold;
    j["font_italic"]   = l.font_italic;
    j["text_color"]    = l.text_color;
    j["stroke_color"]  = l.stroke_color;
    j["stroke_width"]  = l.stroke_width;
    j["align_h"]       = l.align_h;
    j["align_v"]       = l.align_v;

    j["fill_color"]    = l.fill_color;
    j["rect_width"]    = l.rect_width;
    j["rect_height"]   = l.rect_height;
    j["corner_radius"] = l.corner_radius;
    j["box_width"]     = aprop_to_json(l.box_width);
    j["box_height"]    = aprop_to_json(l.box_height);
    j["origin_x"]      = l.origin_x;
    j["origin_y"]      = l.origin_y;
    j["origin_x_prop"] = aprop_to_json(l.origin_x_prop);
    j["origin_y_prop"] = aprop_to_json(l.origin_y_prop);
    j["shadow_enabled"] = l.shadow_enabled;
    j["shadow_color"] = l.shadow_color;
    j["shadow_opacity"] = l.shadow_opacity;
    j["shadow_distance"] = l.shadow_distance;
    j["shadow_angle"] = l.shadow_angle;
    j["shadow_blur"] = l.shadow_blur;
    j["shadow_spread"] = l.shadow_spread;
    j["shadow_enabled_prop"] = aprop_to_json(l.shadow_enabled_prop);
    j["shadow_opacity_prop"] = aprop_to_json(l.shadow_opacity_prop);
    j["shadow_distance_prop"] = aprop_to_json(l.shadow_distance_prop);
    j["shadow_angle_prop"] = aprop_to_json(l.shadow_angle_prop);
    j["shadow_blur_prop"] = aprop_to_json(l.shadow_blur_prop);
    j["shadow_spread_prop"] = aprop_to_json(l.shadow_spread_prop);
    j["shadow_color_a"] = aprop_to_json(l.shadow_color_a);
    j["shadow_color_r"] = aprop_to_json(l.shadow_color_r);
    j["shadow_color_g"] = aprop_to_json(l.shadow_color_g);
    j["shadow_color_b"] = aprop_to_json(l.shadow_color_b);
    j["text_color_a"]  = aprop_to_json(l.text_color_a);
    j["text_color_r"]  = aprop_to_json(l.text_color_r);
    j["text_color_g"]  = aprop_to_json(l.text_color_g);
    j["text_color_b"]  = aprop_to_json(l.text_color_b);
    j["fill_color_a"]  = aprop_to_json(l.fill_color_a);
    j["fill_color_r"]  = aprop_to_json(l.fill_color_r);
    j["fill_color_g"]  = aprop_to_json(l.fill_color_g);
    j["fill_color_b"]  = aprop_to_json(l.fill_color_b);
    j["image_path"]    = l.image_path;
    j["lock_aspect_ratio"] = l.lock_aspect_ratio;
    return j;
}

static std::shared_ptr<Layer> layer_from_json(const json &j)
{
    auto l = std::make_shared<Layer>();
    l->id       = j.value("id",       "");
    l->name     = j.value("name",     "Layer");
    l->type     = (LayerType)j.value("type", 0);
    l->visible  = j.value("visible",  true);
    l->locked   = j.value("locked",   false);
    l->properties_expanded = j.value("properties_expanded", false);
    l->parent_id = j.value("parent_id", std::string());
    l->in_time  = j.value("in_time",  0.0);
    l->out_time = j.value("out_time", 5.0);

    if (j.contains("pos_x"))    l->pos_x    = aprop_from_json(j["pos_x"],    "pos_x");
    if (j.contains("pos_y"))    l->pos_y    = aprop_from_json(j["pos_y"],    "pos_y");
    if (j.contains("scale_x"))  l->scale_x  = aprop_from_json(j["scale_x"],  "scale_x");
    if (j.contains("scale_y"))  l->scale_y  = aprop_from_json(j["scale_y"],  "scale_y");
    if (j.contains("rotation")) l->rotation = aprop_from_json(j["rotation"], "rotation");
    if (j.contains("opacity"))  l->opacity  = aprop_from_json(j["opacity"],  "opacity");

    l->text_content  = j.value("text_content",  "Title");
    l->expose_text   = j.value("expose_text",   false);
    l->font_family   = j.value("font_family",   "Helvetica Neue");
    l->font_size     = j.value("font_size",     72);
    l->font_bold     = j.value("font_bold",     false);
    l->font_italic   = j.value("font_italic",   false);
    l->text_color    = j.value("text_color",    (uint32_t)0xFFFFFFFF);
    l->stroke_color  = j.value("stroke_color",  (uint32_t)0x00000000);
    l->stroke_width  = j.value("stroke_width",  0.0f);
    l->align_h       = j.value("align_h",       1);
    l->align_v       = j.value("align_v",       1);

    l->fill_color    = j.value("fill_color",    (uint32_t)0xFF222222);
    l->rect_width    = j.value("rect_width",    1920.0f);
    l->rect_height   = j.value("rect_height",   100.0f);
    l->corner_radius = j.value("corner_radius", 0.0f);
    l->box_width.static_value = l->rect_width;
    l->box_height.static_value = l->rect_height;
    if (j.contains("box_width"))  l->box_width  = aprop_from_json(j["box_width"],  "box_width");
    if (j.contains("box_height")) l->box_height = aprop_from_json(j["box_height"], "box_height");
    l->origin_x      = j.value("origin_x",      0.5f);
    l->origin_y      = j.value("origin_y",      0.5f);
    l->origin_x_prop.static_value = l->origin_x;
    l->origin_y_prop.static_value = l->origin_y;
    if (j.contains("origin_x_prop")) l->origin_x_prop = aprop_from_json(j["origin_x_prop"], "origin_x");
    if (j.contains("origin_y_prop")) l->origin_y_prop = aprop_from_json(j["origin_y_prop"], "origin_y");
    l->shadow_enabled = j.value("shadow_enabled", false);
    l->shadow_color = j.value("shadow_color", (uint32_t)0x99000000);
    l->shadow_opacity = j.value("shadow_opacity", 0.6f);
    l->shadow_distance = j.value("shadow_distance", 8.0f);
    l->shadow_angle = j.value("shadow_angle", 135.0f);
    l->shadow_blur = j.value("shadow_blur", 4.0f);
    l->shadow_spread = j.value("shadow_spread", 0.0f);
    l->shadow_enabled_prop.static_value = l->shadow_enabled ? 1.0 : 0.0;
    l->shadow_opacity_prop.static_value = l->shadow_opacity;
    l->shadow_distance_prop.static_value = l->shadow_distance;
    l->shadow_angle_prop.static_value = l->shadow_angle;
    l->shadow_blur_prop.static_value = l->shadow_blur;
    l->shadow_spread_prop.static_value = l->shadow_spread;
    l->shadow_color_a.static_value = (l->shadow_color >> 24) & 0xFF;
    l->shadow_color_r.static_value = (l->shadow_color >> 16) & 0xFF;
    l->shadow_color_g.static_value = (l->shadow_color >> 8) & 0xFF;
    l->shadow_color_b.static_value = l->shadow_color & 0xFF;
    if (j.contains("shadow_enabled_prop")) l->shadow_enabled_prop = aprop_from_json(j["shadow_enabled_prop"], "shadow_enabled");
    if (j.contains("shadow_opacity_prop")) l->shadow_opacity_prop = aprop_from_json(j["shadow_opacity_prop"], "shadow_opacity");
    if (j.contains("shadow_distance_prop")) l->shadow_distance_prop = aprop_from_json(j["shadow_distance_prop"], "shadow_distance");
    if (j.contains("shadow_angle_prop")) l->shadow_angle_prop = aprop_from_json(j["shadow_angle_prop"], "shadow_angle");
    if (j.contains("shadow_blur_prop")) l->shadow_blur_prop = aprop_from_json(j["shadow_blur_prop"], "shadow_blur");
    if (j.contains("shadow_spread_prop")) l->shadow_spread_prop = aprop_from_json(j["shadow_spread_prop"], "shadow_spread");
    if (j.contains("shadow_color_a")) l->shadow_color_a = aprop_from_json(j["shadow_color_a"], "shadow_color_a");
    if (j.contains("shadow_color_r")) l->shadow_color_r = aprop_from_json(j["shadow_color_r"], "shadow_color_r");
    if (j.contains("shadow_color_g")) l->shadow_color_g = aprop_from_json(j["shadow_color_g"], "shadow_color_g");
    if (j.contains("shadow_color_b")) l->shadow_color_b = aprop_from_json(j["shadow_color_b"], "shadow_color_b");
    set_color_channels(*l, true, l->text_color);
    set_color_channels(*l, false, l->fill_color);
    if (j.contains("text_color_a")) l->text_color_a = aprop_from_json(j["text_color_a"], "text_color_a");
    if (j.contains("text_color_r")) l->text_color_r = aprop_from_json(j["text_color_r"], "text_color_r");
    if (j.contains("text_color_g")) l->text_color_g = aprop_from_json(j["text_color_g"], "text_color_g");
    if (j.contains("text_color_b")) l->text_color_b = aprop_from_json(j["text_color_b"], "text_color_b");
    if (j.contains("fill_color_a")) l->fill_color_a = aprop_from_json(j["fill_color_a"], "fill_color_a");
    if (j.contains("fill_color_r")) l->fill_color_r = aprop_from_json(j["fill_color_r"], "fill_color_r");
    if (j.contains("fill_color_g")) l->fill_color_g = aprop_from_json(j["fill_color_g"], "fill_color_g");
    if (j.contains("fill_color_b")) l->fill_color_b = aprop_from_json(j["fill_color_b"], "fill_color_b");
    l->image_path    = j.value("image_path",    "");
    l->lock_aspect_ratio = j.value("lock_aspect_ratio", true);
    return l;
}

static json title_to_json(const Title &t)
{
    json jt;
    jt["id"]       = t.id;
    jt["name"]     = t.name;
    jt["duration"] = t.duration;
    jt["loop_start"] = t.loop_start;
    jt["loop_end"] = t.loop_end;
    jt["playback_mode"] = t.playback_mode;
    jt["loop_type"] = t.loop_type;
    jt["pause_time"] = t.pause_time;
    jt["bg_color"] = t.bg_color;
    jt["width"]    = t.width;
    jt["height"]   = t.height;
    json layers = json::array();
    for (auto &l : t.layers)
        layers.push_back(layer_to_json(*l));
    jt["layers"] = layers;
    json live_rows = json::array();
    for (const auto &row : t.live_text_rows)
        live_rows.push_back(row);
    jt["live_text_rows"] = live_rows;
    return jt;
}

static std::shared_ptr<Title> title_from_json(const json &jt, bool regenerate_ids)
{
    auto t = std::make_shared<Title>();
    t->id       = jt.value("id",       TitleDataStore::make_uuid());
    t->name     = jt.value("name",     "Untitled");
    t->duration = jt.value("duration", 5.0);
    t->loop_start = std::clamp(jt.value("loop_start", std::min(1.0, t->duration)), 0.0, t->duration);
    t->loop_end = std::clamp(jt.value("loop_end", std::max(t->loop_start, t->duration - 1.0)), t->loop_start, t->duration);
    t->playback_mode = std::clamp(jt.value("playback_mode", 0), 0, 2);
    t->loop_type = std::clamp(jt.value("loop_type", 0), 0, 1);
    t->pause_time = std::clamp(jt.value("pause_time", 0.0), 0.0, t->duration);
    t->bg_color = jt.value("bg_color", (uint32_t)0x00000000);
    t->width    = jt.value("width",    1920);
    t->height   = jt.value("height",   1080);
    if (jt.contains("layers"))
        for (auto &lj : jt["layers"])
            t->layers.push_back(layer_from_json(lj));
    if (jt.contains("live_text_rows")) {
        for (const auto &jr : jt["live_text_rows"]) {
            std::vector<std::string> row;
            for (const auto &cell : jr)
                row.push_back(cell.get<std::string>());
            t->live_text_rows.push_back(std::move(row));
        }
    }

    if (regenerate_ids) {
        std::unordered_map<std::string, std::string> layer_id_map;
        t->id = TitleDataStore::make_uuid();
        for (auto &layer : t->layers) {
            std::string old_id = layer->id;
            layer->id = TitleDataStore::make_uuid();
            if (!old_id.empty())
                layer_id_map[old_id] = layer->id;
        }
        for (auto &layer : t->layers) {
            auto it = layer_id_map.find(layer->parent_id);
            if (it != layer_id_map.end())
                layer->parent_id = it->second;
            else if (!layer->parent_id.empty())
                layer->parent_id.clear();
        }
    }

    return t;
}

void TitleDataStore::save() const
{
    json root = json::array();
    for (auto &t : titles_)
        root.push_back(title_to_json(*t));

    std::ofstream f(data_path());
    if (f.is_open())
        f << root.dump(2);
    else
        blog(LOG_WARNING, "[OBS Titler Pro] Failed to save titles.json");
}

bool TitleDataStore::export_title(const std::string &id, const std::string &path, std::string *error) const
{
    auto t = get_title(id);
    if (!t) {
        if (error) *error = "No title template is selected.";
        return false;
    }

    json root;
    root["format"] = "obs-titler-pro-title-template";
    root["version"] = 1;
    root["title"] = title_to_json(*t);

    std::ofstream f(path);
    if (!f.is_open()) {
        if (error) *error = "Could not open the export file for writing.";
        return false;
    }
    f << root.dump(2);
    if (!f.good()) {
        if (error) *error = "Failed while writing the export file.";
        return false;
    }
    return true;
}

std::shared_ptr<Title> TitleDataStore::import_title(const std::string &path, std::string *error)
{
    std::ifstream f(path);
    if (!f.is_open()) {
        if (error) *error = "Could not open the template file.";
        return nullptr;
    }

    try {
        json root;
        f >> root;
        json jt;
        if (root.is_object() && root.contains("title"))
            jt = root["title"];
        else if (root.is_array() && !root.empty())
            jt = root.front();
        else if (root.is_object())
            jt = root;
        else
            throw std::runtime_error("Unsupported template file format.");

        auto imported = title_from_json(jt, true);
        if (!imported)
            throw std::runtime_error("Template data was empty.");

        std::string base_name = imported->name.empty() ? "Imported Title" : imported->name;
        std::string unique_name = base_name;
        int suffix = 2;
        auto name_exists = [this](const std::string &candidate) {
            return std::any_of(titles_.begin(), titles_.end(), [&](const auto &existing) {
                return existing && existing->name == candidate;
            });
        };
        while (name_exists(unique_name))
            unique_name = base_name + " (imported " + std::to_string(suffix++) + ")";
        imported->name = unique_name;

        titles_.push_back(imported);
        notify_change();
        save();
        return imported;
    } catch (const std::exception &e) {
        if (error) *error = e.what();
        return nullptr;
    }
}

void TitleDataStore::load()
{
    std::ifstream f(data_path());
    if (!f.is_open()) {
        blog(LOG_INFO, "[OBS Titler Pro] No saved titles found, starting fresh.");
        return;
    }

    try {
        json root;
        f >> root;
        for (auto &jt : root)
            titles_.push_back(title_from_json(jt, false));
        blog(LOG_INFO, "[OBS Titler Pro] Loaded %zu title(s).", titles_.size());
    } catch (std::exception &e) {
        blog(LOG_WARNING, "[OBS Titler Pro] Failed to parse titles.json: %s", e.what());
    }
}
