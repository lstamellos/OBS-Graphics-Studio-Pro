/*
 * title-data.cpp
 */

#include "title-data.h"
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/platform.h>

#include <nlohmann/json.hpp>
#include <fstream>
#include <random>
#include <sstream>
#include <iomanip>
#include <iterator>
#include <cmath>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <stdexcept>
#include <cstdio>
#include <limits>
#include <utility>
#include <cctype>
#include <ctime>

using json = nlohmann::json;

namespace {
constexpr std::streamoff kMaxJsonFileBytes = 512 * 1024 * 1024;
constexpr std::streamoff kMaxEmbeddedAssetBytes = 100 * 1024 * 1024;
constexpr size_t kMaxTitles = 256;
constexpr size_t kMaxLayersPerTitle = 256;
constexpr size_t kMaxKeyframesPerProperty = 2048;
constexpr size_t kMaxLiveTextRows = 256;
constexpr size_t kMaxLiveTextColumns = 32;
constexpr size_t kMaxNameLength = 256;
constexpr size_t kMaxTextLength = 8192;
constexpr size_t kMaxScreenshotBase64Length = 32 * 1024 * 1024;
constexpr double kMaxDuration = 3600.0;
constexpr double kMaxPropertyValue = 100000.0;
constexpr int kMaxCanvasDimension = 16384;

static double finite_or(double value, double fallback)
{
    return std::isfinite(value) ? value : fallback;
}

static std::string current_iso_utc_string()
{
    const std::time_t now = std::time(nullptr);
    std::tm tm_utc{};
#if defined(_WIN32)
    gmtime_s(&tm_utc, &now);
#else
    gmtime_r(&now, &tm_utc);
#endif
    std::ostringstream out;
    out << std::put_time(&tm_utc, "%Y-%m-%dT%H:%M:%SZ");
    return out.str();
}

static std::string title_data_dir()
{
    char *cfg_dir = obs_module_config_path("");
    std::string dir(cfg_dir ? cfg_dir : "");
    bfree(cfg_dir);
    os_mkdirs(dir.c_str());
    return dir;
}

static uint64_t fnv1a64(const std::string &value)
{
    uint64_t hash = 14695981039346656037ull;
    for (unsigned char ch : value) {
        hash ^= ch;
        hash *= 1099511628211ull;
    }
    return hash;
}

static std::string hex64(uint64_t value)
{
    std::ostringstream out;
    out << std::hex << std::setw(16) << std::setfill('0') << value;
    return out.str();
}

static std::string current_scene_collection_name()
{
    char *collection_name = obs_frontend_get_current_scene_collection();
    std::string name(collection_name ? collection_name : "");
    bfree(collection_name);
    if (name.empty())
        name = "unknown-scene-collection";
    return name;
}

static std::string safe_scene_collection_file_stem(const std::string &name)
{
    std::string safe;
    safe.reserve(std::min<size_t>(name.size(), 80));
    for (unsigned char ch : name) {
        if (std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.')
            safe.push_back((char)ch);
        else
            safe.push_back('_');
        if (safe.size() >= 80)
            break;
    }
    if (safe.empty())
        safe = "scene-collection";
    return safe + "-" + hex64(fnv1a64(name));
}

static std::string bounded_string(const json &j, const char *key,
                                  const std::string &fallback,
                                  size_t max_len)
{
    if (!j.is_object() || !j.contains(key) || !j[key].is_string())
        return fallback;
    std::string value = j[key].get<std::string>();
    if (value.size() > max_len)
        value.resize(max_len);
    return value;
}

static const json *object_member(const json &j, const char *key)
{
    if (!j.is_object())
        return nullptr;
    auto it = j.find(key);
    return it == j.end() ? nullptr : &*it;
}

static bool json_bool(const json &j, const char *key, bool fallback)
{
    const json *value = object_member(j, key);
    return value && value->is_boolean() ? value->get<bool>() : fallback;
}

static int json_int(const json &j, const char *key, int fallback)
{
    const json *value = object_member(j, key);
    if (!value || !value->is_number_integer())
        return fallback;
    const int64_t parsed = value->get<int64_t>();
    if (parsed < std::numeric_limits<int>::min() || parsed > std::numeric_limits<int>::max())
        return fallback;
    return (int)parsed;
}

static double json_double(const json &j, const char *key, double fallback)
{
    const json *value = object_member(j, key);
    return value && value->is_number() ? finite_or(value->get<double>(), fallback) : fallback;
}

static uint32_t json_color(const json &j, const char *key, uint32_t fallback)
{
    const json *value = object_member(j, key);
    if (!value)
        return fallback;
    if (value->is_number_unsigned()) {
        const uint64_t parsed = value->get<uint64_t>();
        return parsed <= UINT32_MAX ? (uint32_t)parsed : fallback;
    }
    if (value->is_number_integer()) {
        const int64_t parsed = value->get<int64_t>();
        return parsed >= 0 && parsed <= UINT32_MAX ? (uint32_t)parsed : fallback;
    }
    return fallback;
}


static bool file_exists(const std::string &path)
{
    std::ifstream f(path, std::ios::binary);
    return f.is_open();
}

static std::string file_name_from_path(const std::string &path)
{
    const size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

static std::string sanitize_asset_file_name(const std::string &file_name)
{
    std::string sanitized;
    sanitized.reserve(file_name.size());
    for (unsigned char ch : file_name) {
        if (std::isalnum(ch) || ch == '.' || ch == '-' || ch == '_')
            sanitized.push_back((char)ch);
        else
            sanitized.push_back('_');
    }
    while (!sanitized.empty() && sanitized.front() == '.')
        sanitized.erase(sanitized.begin());
    if (sanitized.empty())
        sanitized = "image.bin";
    if (sanitized.size() > 160)
        sanitized.resize(160);
    return sanitized;
}

static std::string lower_extension(const std::string &file_name)
{
    const size_t dot = file_name.find_last_of('.');
    if (dot == std::string::npos)
        return {};
    std::string ext = file_name.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char ch) { return (char)std::tolower(ch); });
    return ext;
}

static std::string mime_type_for_file_name(const std::string &file_name)
{
    const std::string ext = lower_extension(file_name);
    if (ext == "png") return "image/png";
    if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
    if (ext == "gif") return "image/gif";
    if (ext == "webp") return "image/webp";
    if (ext == "bmp") return "image/bmp";
    if (ext == "svg" || ext == "svgz") return "image/svg+xml";
    return "application/octet-stream";
}

static bool read_binary_file(const std::string &path, std::string &out, std::streamoff max_bytes, std::string *error)
{
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) {
        if (error) *error = "Could not open asset file: " + path;
        return false;
    }

    f.seekg(0, std::ios::end);
    const std::streamoff size = f.tellg();
    if (size < 0 || size > max_bytes) {
        if (error) *error = "Asset file is too large to embed: " + path;
        return false;
    }
    f.seekg(0, std::ios::beg);

    out.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (!f.good() && !f.eof()) {
        if (error) *error = "Failed while reading asset file: " + path;
        return false;
    }
    return true;
}

static std::string base64_encode(const std::string &data)
{
    static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string encoded;
    encoded.reserve(((data.size() + 2) / 3) * 4);

    int value = 0;
    int bits = -6;
    for (unsigned char ch : data) {
        value = (value << 8) + ch;
        bits += 8;
        while (bits >= 0) {
            encoded.push_back(table[(value >> bits) & 0x3F]);
            bits -= 6;
        }
    }
    if (bits > -6)
        encoded.push_back(table[((value << 8) >> (bits + 8)) & 0x3F]);
    while (encoded.size() % 4)
        encoded.push_back('=');
    return encoded;
}

static bool base64_decode(const std::string &encoded, std::string &out)
{
    static const std::string table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::vector<int> reverse(256, -1);
    for (int i = 0; i < (int)table.size(); ++i)
        reverse[(unsigned char)table[i]] = i;

    out.clear();
    int value = 0;
    int bits = -8;
    for (unsigned char ch : encoded) {
        if (std::isspace(ch))
            continue;
        if (ch == '=')
            break;
        if (reverse[ch] == -1)
            return false;
        value = (value << 6) + reverse[ch];
        bits += 6;
        if (bits >= 0) {
            out.push_back((char)((value >> bits) & 0xFF));
            bits -= 8;
        }
    }
    return true;
}

static uint64_t fnv1a_64(const std::string &data)
{
    uint64_t hash = 1469598103934665603ULL;
    for (unsigned char ch : data) {
        hash ^= ch;
        hash *= 1099511628211ULL;
    }
    return hash;
}

static std::string hex_u64(uint64_t value)
{
    std::ostringstream ss;
    ss << std::hex << std::setfill('0') << std::setw(16) << value;
    return ss.str();
}

static std::string embedded_assets_dir()
{
    char *cfg_dir = obs_module_config_path("");
    std::string dir(cfg_dir);
    bfree(cfg_dir);
    const std::string assets = dir + "/assets";
    os_mkdirs(assets.c_str());
    return assets;
}

static bool attach_embedded_image_asset(const Layer &layer, json &j, bool required, std::string *error)
{
    if (layer.type != LayerType::Image || layer.image_path.empty())
        return true;

    std::string data;
    if (!read_binary_file(layer.image_path, data, kMaxEmbeddedAssetBytes, error))
        return !required;

    const std::string file_name = sanitize_asset_file_name(file_name_from_path(layer.image_path));
    json asset;
    asset["file_name"] = file_name;
    asset["mime_type"] = mime_type_for_file_name(file_name);
    asset["size"] = data.size();
    asset["hash"] = hex_u64(fnv1a_64(data));
    asset["data_base64"] = base64_encode(data);
    j["embedded_image"] = std::move(asset);
    return true;
}

static bool restore_embedded_image_asset(const json &j, std::string &image_path)
{
    const json *asset = object_member(j, "embedded_image");
    if (!asset || !asset->is_object())
        return false;

    const std::string data64 = bounded_string(*asset, "data_base64", "", (size_t)kMaxEmbeddedAssetBytes * 2);
    if (data64.empty())
        return false;

    std::string data;
    if (!base64_decode(data64, data) || data.empty() || (std::streamoff)data.size() > kMaxEmbeddedAssetBytes)
        return false;

    std::string file_name = sanitize_asset_file_name(bounded_string(*asset, "file_name", "image.bin", kMaxNameLength));
    const std::string hash = hex_u64(fnv1a_64(data));
    file_name = hash + "-" + file_name;

    const std::string path = embedded_assets_dir() + "/" + file_name;
    if (!file_exists(path)) {
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        if (!f.is_open())
            return false;
        f.write(data.data(), (std::streamsize)data.size());
        if (!f.good()) {
            std::remove(path.c_str());
            return false;
        }
    }

    image_path = path;
    return true;
}

static bool read_json_file(const std::string &path, json &out, std::string *error)
{
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) {
        if (error) *error = "Could not open the file.";
        return false;
    }

    f.seekg(0, std::ios::end);
    const std::streamoff size = f.tellg();
    if (size < 0 || size > kMaxJsonFileBytes) {
        if (error) *error = "File is too large for a title template.";
        return false;
    }
    f.seekg(0, std::ios::beg);

    try {
        f >> out;
    } catch (const std::exception &e) {
        if (error) *error = e.what();
        return false;
    }
    return true;
}

static void ensure_unique_title_id(const std::shared_ptr<Title> &title,
                                   std::unordered_set<std::string> &seen)
{
    if (!title)
        return;
    if (title->id.empty() || seen.find(title->id) != seen.end())
        title->id = TitleDataStore::make_uuid();
    seen.insert(title->id);

    std::unordered_set<std::string> layer_ids;
    for (auto &layer : title->layers) {
        if (!layer)
            continue;
        if (layer->id.empty() || layer_ids.find(layer->id) != layer_ids.end())
            layer->id = TitleDataStore::make_uuid();
        layer_ids.insert(layer->id);
    }
    for (auto &layer : title->layers) {
        if (!layer || layer->parent_id.empty())
            continue;
        if (layer_ids.find(layer->parent_id) == layer_ids.end())
            layer->parent_id.clear();
    }
}
} // namespace

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

static void set_background_color_channels(Layer &l, uint32_t argb)
{
    l.background_color_a.static_value = (argb >> 24) & 0xFF;
    l.background_color_r.static_value = (argb >> 16) & 0xFF;
    l.background_color_g.static_value = (argb >> 8) & 0xFF;
    l.background_color_b.static_value = argb & 0xFF;
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
    if (!std::isfinite(t)) return static_value;
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
        return bezierY(x, cx1, cy1, cx2, cy2);
    default: return x;
    }
    (void)cx1; (void)cx2; // used by full bezier solver if needed
}

/* Cubic-bezier Y for a given X using P0=(0,0), P3=(1,1). */
double AnimatedProperty::bezierY(double x, float cx1, float cy1,
                                 float cx2, float cy2)
{
    auto sample = [](double t, double p1, double p2) {
        double inv = 1.0 - t;
        return 3.0 * inv * inv * t * p1 +
               3.0 * inv * t * t * p2 +
               t * t * t;
    };
    auto slope = [](double t, double p1, double p2) {
        double inv = 1.0 - t;
        return 3.0 * inv * inv * p1 +
               6.0 * inv * t * (p2 - p1) +
               3.0 * t * t * (1.0 - p2);
    };

    x = std::clamp(x, 0.0, 1.0);
    cx1 = std::clamp(cx1, 0.0f, 1.0f);
    cx2 = std::clamp(cx2, 0.0f, 1.0f);

    double t = x;
    for (int i = 0; i < 8; ++i) {
        double dx = sample(t, cx1, cx2) - x;
        double d = slope(t, cx1, cx2);
        if (std::abs(dx) < 1e-6) break;
        if (std::abs(d) < 1e-6) break;
        t = std::clamp(t - dx / d, 0.0, 1.0);
    }

    double lo = 0.0, hi = 1.0;
    for (int i = 0; i < 12; ++i) {
        double bx = sample(t, cx1, cx2);
        if (std::abs(bx - x) < 1e-6) break;
        if (bx < x) lo = t; else hi = t;
        t = 0.5 * (lo + hi);
    }

    return std::clamp(sample(t, cy1, cy2), 0.0, 1.0);
}

/* ══════════════════════════════════════════════════════════════════
 *  Title helpers
 * ══════════════════════════════════════════════════════════════════ */
std::shared_ptr<Layer> Title::find_layer(const std::string &lid) const
{
    for (auto &l : layers)
        if (l && l->id == lid) return l;
    return nullptr;
}

void Title::add_layer(std::shared_ptr<Layer> l)
{
    if (l)
        layers.push_back(l);
}

void Title::remove_layer(const std::string &lid)
{
    layers.erase(
        std::remove_if(layers.begin(), layers.end(),
                       [&](auto &l){ return !l || l->id == lid; }),
        layers.end());
}

void Title::move_layer(const std::string &lid, int delta)
{
    auto it = std::find_if(layers.begin(), layers.end(),
                           [&](auto &l){ return l && l->id == lid; });
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

std::vector<std::shared_ptr<Title>> TitleDataStore::titles() const
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return titles_;
}

void TitleDataStore::on_change(ChangeCallback cb)
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    change_cbs_.push_back(std::move(cb));
}

void TitleDataStore::notify_change()
{
    touch_runtime_change();

    std::vector<ChangeCallback> callbacks;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        callbacks = change_cbs_;
    }

    for (auto &cb : callbacks) cb();
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
    t->creation_date = current_iso_utc_string();

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

    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        titles_.push_back(t);
    }
    notify_change();
    return t;
}

std::shared_ptr<Title> TitleDataStore::get_title(const std::string &id) const
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    for (auto &t : titles_)
        if (t->id == id) return t;
    return nullptr;
}

void TitleDataStore::delete_title(const std::string &id)
{
    bool deleted = false;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        const auto old_size = titles_.size();
        titles_.erase(
            std::remove_if(titles_.begin(), titles_.end(),
                           [&](auto &t){ return t && t->id == id; }),
            titles_.end());
        deleted = titles_.size() != old_size;
    }

    if (deleted)
        notify_change();
}

void TitleDataStore::rename_title(const std::string &id, const std::string &n)
{
    bool renamed = false;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        for (auto &t : titles_) {
            if (t && t->id == id) {
                t->name = n;
                renamed = true;
                break;
            }
        }
    }

    if (renamed)
        notify_change();
}

/* ── persistence ──────────────────────────────────────────────────── */
std::string TitleDataStore::data_path()
{
    const std::string dir = title_data_dir() + "/scene-collection-titles";
    os_mkdirs(dir.c_str());
    const std::string collection_name = current_scene_collection_name();
    return dir + "/" + safe_scene_collection_file_stem(collection_name) + ".json";
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
    if (!j.is_object())
        return k;
    k.time = std::clamp(finite_or(json_double(j, "time", 0.0), 0.0), 0.0, kMaxDuration);
    k.value = std::clamp(finite_or(json_double(j, "value", 0.0), 0.0), -kMaxPropertyValue, kMaxPropertyValue);
    k.easing = (EasingType)std::clamp(json_int(j, "easing", 0), 0, (int)EasingType::Hold);
    k.cx1 = std::clamp(finite_or(json_double(j, "cx1", 0.333), 0.333), 0.0, 1.0);
    k.cy1 = std::clamp(finite_or(json_double(j, "cy1", 0.0), 0.0), 0.0, 1.0);
    k.cx2 = std::clamp(finite_or(json_double(j, "cx2", 0.667), 0.667), 0.0, 1.0);
    k.cy2 = std::clamp(finite_or(json_double(j, "cy2", 1.0), 1.0), 0.0, 1.0);
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
    p.name = name;
    if (!j.is_object())
        return p;

    p.static_value = std::clamp(finite_or(json_double(j, "static_value", 0.0), 0.0),
                                -kMaxPropertyValue, kMaxPropertyValue);
    if (j.contains("keyframes") && j["keyframes"].is_array()) {
        const size_t count = std::min(j["keyframes"].size(), kMaxKeyframesPerProperty);
        p.keyframes.reserve(count);
        for (size_t i = 0; i < count; ++i)
            p.keyframes.push_back(keyframe_from_json(j["keyframes"][i]));
        std::sort(p.keyframes.begin(), p.keyframes.end(),
                  [](const Keyframe &a, const Keyframe &b) { return a.time < b.time; });
    }
    return p;
}

static json layer_to_json(const Layer &l, bool include_embedded_assets = true,
                          bool require_embedded_assets = false, std::string *error = nullptr,
                          bool *asset_embed_failed = nullptr)
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
    j["scale_lock"] = l.scale_lock;
    j["rotation"] = aprop_to_json(l.rotation);
    j["opacity"]  = aprop_to_json(l.opacity);

    j["text_content"]  = l.text_content;
    j["clock_format"]  = l.clock_format;
    j["expose_text"]   = l.expose_text;
    j["font_family"]   = l.font_family;
    j["font_style"]    = l.font_style;
    j["font_size"]     = l.font_size;
    j["font_bold"]     = l.font_bold;
    j["font_italic"]   = l.font_italic;
    j["font_kerning"]  = l.font_kerning;
    j["kerning_mode"]  = l.kerning_mode;
    j["manual_kerning"] = l.manual_kerning;
    j["text_leading"]  = l.text_leading;
    j["char_tracking"] = l.char_tracking;
    j["char_scale_x"]  = l.char_scale_x;
    j["char_scale_y"]  = l.char_scale_y;
    j["baseline_shift"] = l.baseline_shift;
    j["text_style"]    = l.text_style;
    j["text_underline"] = l.text_underline;
    j["text_strikethrough"] = l.text_strikethrough;
    j["text_ligatures"] = l.text_ligatures;
    j["text_stylistic_alternates"] = l.text_stylistic_alternates;
    j["text_fractions"] = l.text_fractions;
    j["text_opentype_features"] = l.text_opentype_features;
    j["text_language"] = l.text_language;
    j["text_overflow_mode"] = l.text_overflow_mode;
    j["text_fit_min_scale"] = l.text_fit_min_scale;
    j["text_box_width_to_text"] = l.text_box_width_to_text;
    j["text_box_height_to_text"] = l.text_box_height_to_text;
    j["max_text_box_width"] = l.max_text_box_width;
    j["max_text_box_height"] = l.max_text_box_height;
    j["ticker_style"] = l.ticker_style;
    j["ticker_speed"] = l.ticker_speed;
    j["ticker_line_hold"] = l.ticker_line_hold;
    j["ticker_direction"] = l.ticker_direction;
    j["text_color"]    = l.text_color;
    j["outline_enabled"] = l.outline_enabled;
    j["stroke_color"]  = l.stroke_color;
    j["stroke_width"]  = l.stroke_width;
    j["outline_opacity"] = l.outline_opacity;
    j["outline_join_style"] = l.outline_join_style;
    j["outline_on_front"] = l.outline_on_front;
    j["outline_antialias"] = l.outline_antialias;
    j["align_h"]       = l.align_h;
    j["align_v"]       = l.align_v;

    j["fill_color"]    = l.fill_color;
    j["fill_type"]     = l.fill_type;
    j["gradient_type"] = l.gradient_type;
    j["gradient_start_color"] = l.gradient_start_color;
    j["gradient_end_color"] = l.gradient_end_color;
    j["gradient_start_pos"] = l.gradient_start_pos;
    j["gradient_end_pos"] = l.gradient_end_pos;
    j["gradient_start_opacity"] = l.gradient_start_opacity;
    j["gradient_end_opacity"] = l.gradient_end_opacity;
    j["gradient_opacity"] = l.gradient_opacity;
    j["gradient_angle"] = l.gradient_angle;
    j["gradient_center_x"] = l.gradient_center_x;
    j["gradient_center_y"] = l.gradient_center_y;
    j["gradient_scale"] = l.gradient_scale;
    j["gradient_focal_x"] = l.gradient_focal_x;
    j["gradient_focal_y"] = l.gradient_focal_y;
    j["background_enabled"] = l.background_enabled;
    j["background_color"] = l.background_color;
    j["background_opacity"] = l.background_opacity;
    j["background_padding"] = l.background_padding_x;
    j["background_padding_x"] = l.background_padding_x;
    j["background_padding_y"] = l.background_padding_y;
    j["background_corner_radius"] = l.background_corner_radius;
    j["background_enabled_prop"] = aprop_to_json(l.background_enabled_prop);
    j["background_opacity_prop"] = aprop_to_json(l.background_opacity_prop);
    j["background_padding_x_prop"] = aprop_to_json(l.background_padding_x_prop);
    j["background_padding_y_prop"] = aprop_to_json(l.background_padding_y_prop);
    j["background_corner_radius_prop"] = aprop_to_json(l.background_corner_radius_prop);
    j["background_color_a"] = aprop_to_json(l.background_color_a);
    j["background_color_r"] = aprop_to_json(l.background_color_r);
    j["background_color_g"] = aprop_to_json(l.background_color_g);
    j["background_color_b"] = aprop_to_json(l.background_color_b);
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
    if (include_embedded_assets && !attach_embedded_image_asset(l, j, require_embedded_assets, error)) {
        if (asset_embed_failed)
            *asset_embed_failed = true;
    }
    j["lock_aspect_ratio"] = l.lock_aspect_ratio;
    return j;
}

static std::shared_ptr<Layer> layer_from_json(const json &j, bool require_embedded_assets = false,
                                               std::string *error = nullptr)
{
    auto l = std::make_shared<Layer>();
    if (!j.is_object())
        return l;

    l->id       = bounded_string(j, "id", "", kMaxNameLength);
    l->name     = bounded_string(j, "name", "Layer", kMaxNameLength);
    l->type     = (LayerType)std::clamp(json_int(j, "type", 0), 0, (int)LayerType::Ticker);
    l->visible  = json_bool(j, "visible", true);
    l->locked   = json_bool(j, "locked", false);
    l->properties_expanded = json_bool(j, "properties_expanded", false);
    l->parent_id = bounded_string(j, "parent_id", "", kMaxNameLength);
    l->in_time  = std::clamp(finite_or(json_double(j, "in_time", 0.0), 0.0), 0.0, kMaxDuration);
    l->out_time = std::clamp(finite_or(json_double(j, "out_time", 5.0), 5.0), l->in_time, kMaxDuration);

    if (j.contains("pos_x"))    l->pos_x    = aprop_from_json(j["pos_x"],    "pos_x");
    if (j.contains("pos_y"))    l->pos_y    = aprop_from_json(j["pos_y"],    "pos_y");
    if (j.contains("scale_x"))  l->scale_x  = aprop_from_json(j["scale_x"],  "scale_x");
    if (j.contains("scale_y"))  l->scale_y  = aprop_from_json(j["scale_y"],  "scale_y");
    l->scale_lock = json_bool(j, "scale_lock", true);
    if (j.contains("rotation")) l->rotation = aprop_from_json(j["rotation"], "rotation");
    if (j.contains("opacity"))  l->opacity  = aprop_from_json(j["opacity"],  "opacity");
    l->scale_x.static_value = std::clamp(l->scale_x.static_value, -100.0, 100.0);
    l->scale_y.static_value = std::clamp(l->scale_y.static_value, -100.0, 100.0);
    l->opacity.static_value = std::clamp(l->opacity.static_value, 0.0, 1.0);

    l->text_content  = bounded_string(j, "text_content", "Title", kMaxTextLength);
    l->clock_format  = bounded_string(j, "clock_format", "H:i:s", kMaxNameLength);
    l->expose_text   = json_bool(j, "expose_text", false);
    l->font_family   = bounded_string(j, "font_family", "Helvetica Neue", kMaxNameLength);
    l->font_style    = bounded_string(j, "font_style", "Regular", kMaxNameLength);
    l->font_size     = std::clamp(json_int(j, "font_size", 72), 1, 512);
    l->font_bold     = json_bool(j, "font_bold", false);
    l->font_italic   = json_bool(j, "font_italic", false);
    l->font_kerning  = json_bool(j, "font_kerning", true);
    l->kerning_mode  = std::clamp(json_int(j, "kerning_mode", 0), 0, 2);
    l->manual_kerning = (float)std::clamp(finite_or(json_double(j, "manual_kerning", 0.0), 0.0), -1000.0, 1000.0);
    l->text_leading  = (float)std::clamp(finite_or(json_double(j, "text_leading", 0.0), 0.0), -1000.0, 1000.0);
    l->char_tracking = (float)std::clamp(finite_or(json_double(j, "char_tracking", 0.0), 0.0), -1000.0, 1000.0);
    l->char_scale_x  = (float)std::clamp(finite_or(json_double(j, "char_scale_x", 1.0), 1.0), 0.01, 100.0);
    l->char_scale_y  = (float)std::clamp(finite_or(json_double(j, "char_scale_y", 1.0), 1.0), 0.01, 100.0);
    l->baseline_shift = (float)std::clamp(finite_or(json_double(j, "baseline_shift", 0.0), 0.0), -1000.0, 1000.0);
    l->text_style    = std::clamp(json_int(j, "text_style", 0), 0, 4);
    l->text_underline = json_bool(j, "text_underline", false);
    l->text_strikethrough = json_bool(j, "text_strikethrough", false);
    l->text_ligatures = json_bool(j, "text_ligatures", true);
    l->text_stylistic_alternates = json_bool(j, "text_stylistic_alternates", false);
    l->text_fractions = json_bool(j, "text_fractions", false);
    l->text_opentype_features = json_bool(j, "text_opentype_features", false);
    l->text_language = bounded_string(j, "text_language", "English", kMaxNameLength);
    l->text_overflow_mode = std::clamp(json_int(j, "text_overflow_mode", 0), 0, 2);
    l->text_fit_min_scale = (float)std::clamp(finite_or(json_double(j, "text_fit_min_scale", 0.5), 0.5), 0.05, 1.0);
    l->text_box_width_to_text = json_bool(j, "text_box_width_to_text", false);
    l->text_box_height_to_text = json_bool(j, "text_box_height_to_text", false);
    l->max_text_box_width = (float)std::clamp(finite_or(json_double(j, "max_text_box_width", 1920.0), 1920.0), 1.0, (double)kMaxCanvasDimension);
    l->max_text_box_height = (float)std::clamp(finite_or(json_double(j, "max_text_box_height", 1080.0), 1080.0), 1.0, (double)kMaxCanvasDimension);
    l->ticker_style = std::clamp(json_int(j, "ticker_style", 0), 0, 2);
    l->ticker_speed = std::clamp(finite_or(json_double(j, "ticker_speed", 120.0), 120.0), 0.0, 10000.0);
    l->ticker_line_hold = std::clamp(finite_or(json_double(j, "ticker_line_hold", 2.0), 2.0), 0.0, kMaxDuration);
    l->ticker_direction = std::clamp(json_int(j, "ticker_direction", 1), 0, 1);
    l->text_color    = json_color(j, "text_color", (uint32_t)0xFFFFFFFF);
    l->stroke_color  = json_color(j, "stroke_color", (uint32_t)0xFF000000);
    l->stroke_width  = std::clamp(finite_or(json_double(j, "stroke_width", 0.0), 0.0), 0.0, 512.0);
    l->outline_enabled = json_bool(j, "outline_enabled", l->stroke_width > 0.0f);
    l->outline_opacity = std::clamp(finite_or(json_double(j, "outline_opacity", 1.0), 1.0), 0.0, 1.0);
    l->outline_join_style = std::clamp(json_int(j, "outline_join_style", 1), 0, 2);
    l->outline_on_front = json_bool(j, "outline_on_front", true);
    l->outline_antialias = json_bool(j, "outline_antialias", true);
    l->align_h       = std::clamp(json_int(j, "align_h", 1), 0, 2);
    l->align_v       = std::clamp(json_int(j, "align_v", 1), 0, 2);

    l->fill_color    = json_color(j, "fill_color", (uint32_t)0xFF222222);
    l->fill_type     = std::clamp(json_int(j, "fill_type", 0), 0, 1);
    l->gradient_type = std::clamp(json_int(j, "gradient_type", 0), 0, 1);
    l->gradient_start_color = json_color(j, "gradient_start_color", (uint32_t)0xFF4B6EA8);
    l->gradient_end_color = json_color(j, "gradient_end_color", (uint32_t)0xFF1B1B1B);
    l->gradient_start_pos = (float)std::clamp(finite_or(json_double(j, "gradient_start_pos", 0.0), 0.0), 0.0, 1.0);
    l->gradient_end_pos = (float)std::clamp(finite_or(json_double(j, "gradient_end_pos", 1.0), 1.0), 0.0, 1.0);
    l->gradient_start_opacity = (float)std::clamp(finite_or(json_double(j, "gradient_start_opacity", 1.0), 1.0), 0.0, 1.0);
    l->gradient_end_opacity = (float)std::clamp(finite_or(json_double(j, "gradient_end_opacity", 1.0), 1.0), 0.0, 1.0);
    l->gradient_opacity = (float)std::clamp(finite_or(json_double(j, "gradient_opacity", 1.0), 1.0), 0.0, 1.0);
    l->gradient_angle = (float)finite_or(json_double(j, "gradient_angle", 0.0), 0.0);
    l->gradient_center_x = (float)std::clamp(finite_or(json_double(j, "gradient_center_x", 0.5), 0.5), 0.0, 1.0);
    l->gradient_center_y = (float)std::clamp(finite_or(json_double(j, "gradient_center_y", 0.5), 0.5), 0.0, 1.0);
    l->gradient_scale = (float)std::clamp(finite_or(json_double(j, "gradient_scale", 1.0), 1.0), 0.01, 10.0);
    l->gradient_focal_x = (float)std::clamp(finite_or(json_double(j, "gradient_focal_x", l->gradient_center_x), l->gradient_center_x), 0.0, 1.0);
    l->gradient_focal_y = (float)std::clamp(finite_or(json_double(j, "gradient_focal_y", l->gradient_center_y), l->gradient_center_y), 0.0, 1.0);
    l->background_enabled = json_bool(j, "background_enabled", false);
    l->background_color = json_color(j, "background_color", (uint32_t)0xFF000000);
    l->background_opacity = (float)std::clamp(finite_or(json_double(j, "background_opacity", 0.35), 0.35), 0.0, 1.0);
    const double legacy_padding = finite_or(json_double(j, "background_padding", 0.0), 0.0);
    l->background_padding_x = (float)std::clamp(finite_or(json_double(j, "background_padding_x", legacy_padding), legacy_padding), 0.0, (double)kMaxCanvasDimension);
    l->background_padding_y = (float)std::clamp(finite_or(json_double(j, "background_padding_y", legacy_padding), legacy_padding), 0.0, (double)kMaxCanvasDimension);
    l->background_corner_radius = (float)std::clamp(finite_or(json_double(j, "background_corner_radius", 0.0), 0.0), 0.0, (double)kMaxCanvasDimension);
    l->background_enabled_prop.static_value = l->background_enabled ? 1.0 : 0.0;
    l->background_opacity_prop.static_value = l->background_opacity;
    l->background_padding_x_prop.static_value = l->background_padding_x;
    l->background_padding_y_prop.static_value = l->background_padding_y;
    l->background_corner_radius_prop.static_value = l->background_corner_radius;
    set_background_color_channels(*l, l->background_color);
    if (j.contains("background_enabled_prop")) l->background_enabled_prop = aprop_from_json(j["background_enabled_prop"], "background_enabled");
    if (j.contains("background_opacity_prop")) l->background_opacity_prop = aprop_from_json(j["background_opacity_prop"], "background_opacity");
    if (j.contains("background_padding_x_prop")) l->background_padding_x_prop = aprop_from_json(j["background_padding_x_prop"], "background_padding_x");
    if (j.contains("background_padding_y_prop")) l->background_padding_y_prop = aprop_from_json(j["background_padding_y_prop"], "background_padding_y");
    if (j.contains("background_corner_radius_prop")) l->background_corner_radius_prop = aprop_from_json(j["background_corner_radius_prop"], "background_corner_radius");
    if (j.contains("background_color_a")) l->background_color_a = aprop_from_json(j["background_color_a"], "background_color_a");
    if (j.contains("background_color_r")) l->background_color_r = aprop_from_json(j["background_color_r"], "background_color_r");
    if (j.contains("background_color_g")) l->background_color_g = aprop_from_json(j["background_color_g"], "background_color_g");
    if (j.contains("background_color_b")) l->background_color_b = aprop_from_json(j["background_color_b"], "background_color_b");
    l->rect_width    = std::clamp(finite_or(json_double(j, "rect_width", 1920.0), 1920.0), 0.0, (double)kMaxCanvasDimension);
    l->rect_height   = std::clamp(finite_or(json_double(j, "rect_height", 100.0), 100.0), 0.0, (double)kMaxCanvasDimension);
    l->corner_radius = std::clamp(finite_or(json_double(j, "corner_radius", 0.0), 0.0), 0.0, (double)kMaxCanvasDimension);
    l->box_width.static_value = l->rect_width;
    l->box_height.static_value = l->rect_height;
    if (j.contains("box_width"))  l->box_width  = aprop_from_json(j["box_width"],  "box_width");
    if (j.contains("box_height")) l->box_height = aprop_from_json(j["box_height"], "box_height");
    l->box_width.static_value = std::clamp(l->box_width.static_value, 0.0, (double)kMaxCanvasDimension);
    l->box_height.static_value = std::clamp(l->box_height.static_value, 0.0, (double)kMaxCanvasDimension);
    l->origin_x      = std::clamp(finite_or(json_double(j, "origin_x", 0.5), 0.5), 0.0, 1.0);
    l->origin_y      = std::clamp(finite_or(json_double(j, "origin_y", 0.5), 0.5), 0.0, 1.0);
    l->origin_x_prop.static_value = l->origin_x;
    l->origin_y_prop.static_value = l->origin_y;
    if (j.contains("origin_x_prop")) l->origin_x_prop = aprop_from_json(j["origin_x_prop"], "origin_x");
    if (j.contains("origin_y_prop")) l->origin_y_prop = aprop_from_json(j["origin_y_prop"], "origin_y");
    l->origin_x_prop.static_value = std::clamp(l->origin_x_prop.static_value, 0.0, 1.0);
    l->origin_y_prop.static_value = std::clamp(l->origin_y_prop.static_value, 0.0, 1.0);
    l->shadow_enabled = json_bool(j, "shadow_enabled", false);
    l->shadow_color = json_color(j, "shadow_color", (uint32_t)0x99000000);
    l->shadow_opacity = std::clamp(finite_or(json_double(j, "shadow_opacity", 0.6), 0.6), 0.0, 1.0);
    l->shadow_distance = std::clamp(finite_or(json_double(j, "shadow_distance", 8.0), 8.0), 0.0, 4096.0);
    l->shadow_angle = finite_or(json_double(j, "shadow_angle", 135.0), 135.0);
    l->shadow_blur = std::clamp(finite_or(json_double(j, "shadow_blur", 4.0), 4.0), 0.0, 512.0);
    l->shadow_spread = std::clamp(finite_or(json_double(j, "shadow_spread", 0.0), 0.0), 0.0, 512.0);
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
    l->image_path    = bounded_string(j, "image_path", "", 4096);
    if (object_member(j, "embedded_image") && !restore_embedded_image_asset(j, l->image_path) && require_embedded_assets) {
        if (error) *error = "Could not restore an embedded image asset from the template file.";
    }
    l->lock_aspect_ratio = json_bool(j, "lock_aspect_ratio", true);
    return l;
}

static json title_to_json(const Title &t, bool include_embedded_assets = true,
                          bool require_embedded_assets = false, std::string *error = nullptr)
{
    json jt;
    jt["id"]       = t.id;
    jt["name"]     = t.name;
    if (!t.description.empty()) jt["description"] = t.description;
    if (!t.creator.empty()) jt["creator"] = t.creator;
    if (!t.creation_date.empty()) jt["creation_date"] = t.creation_date;
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
    for (auto &l : t.layers) {
        bool asset_embed_failed = false;
        layers.push_back(layer_to_json(*l, include_embedded_assets, require_embedded_assets, error, &asset_embed_failed));
        if (require_embedded_assets && asset_embed_failed) {
            if (error && error->empty())
                *error = "Could not embed an image asset in the template file.";
            return {};
        }
    }
    jt["layers"] = layers;
    json live_rows = json::array();
    for (const auto &row : t.live_text_rows)
        live_rows.push_back(row);
    jt["live_text_rows"] = live_rows;
    jt["live_text_column_order"] = t.live_text_column_order;
    jt["live_text_header_state"] = t.live_text_header_state;
    jt["external_data_enabled"] = t.external_data_enabled;
    if (!t.preview_screenshot_png_base64.empty())
        jt["preview_screenshot_png_base64"] = t.preview_screenshot_png_base64;
    return jt;
}

static std::shared_ptr<Title> title_from_json(const json &jt, bool regenerate_ids,
                                               bool require_embedded_assets = false, std::string *error = nullptr)
{
    auto t = std::make_shared<Title>();
    if (!jt.is_object())
        return t;

    t->id       = bounded_string(jt, "id", TitleDataStore::make_uuid(), kMaxNameLength);
    t->name     = bounded_string(jt, "name", "Untitled", kMaxNameLength);
    t->description = bounded_string(jt, "description", "", kMaxTextLength);
    t->creator = bounded_string(jt, "creator", "", kMaxNameLength);
    t->creation_date = bounded_string(jt, "creation_date", "", kMaxNameLength);
    t->duration = std::clamp(finite_or(json_double(jt, "duration", 5.0), 5.0), 0.1, kMaxDuration);
    t->loop_start = std::clamp(finite_or(json_double(jt, "loop_start", std::min(1.0, t->duration)), 0.0), 0.0, t->duration);
    t->loop_end = std::clamp(finite_or(json_double(jt, "loop_end", std::max(t->loop_start, t->duration - 1.0)), t->duration), t->loop_start, t->duration);
    t->playback_mode = std::clamp(json_int(jt, "playback_mode", 0), 0, 2);
    t->loop_type = std::clamp(json_int(jt, "loop_type", 0), 0, 1);
    t->pause_time = std::clamp(finite_or(json_double(jt, "pause_time", 0.0), 0.0), 0.0, t->duration);
    t->bg_color = json_color(jt, "bg_color", (uint32_t)0x00000000);
    t->width    = std::clamp(json_int(jt, "width", 1920), 1, kMaxCanvasDimension);
    t->height   = std::clamp(json_int(jt, "height", 1080), 1, kMaxCanvasDimension);
    if (jt.contains("layers") && jt["layers"].is_array()) {
        const size_t count = std::min(jt["layers"].size(), kMaxLayersPerTitle);
        t->layers.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            t->layers.push_back(layer_from_json(jt["layers"][i], require_embedded_assets, error));
            if (require_embedded_assets && error && !error->empty())
                return t;
        }
    }
    if (jt.contains("live_text_rows") && jt["live_text_rows"].is_array()) {
        const size_t row_count = std::min(jt["live_text_rows"].size(), kMaxLiveTextRows);
        for (size_t r = 0; r < row_count; ++r) {
            const auto &jr = jt["live_text_rows"][r];
            if (!jr.is_array())
                continue;
            std::vector<std::string> row;
            const size_t col_count = std::min(jr.size(), kMaxLiveTextColumns);
            row.reserve(col_count);
            for (size_t c = 0; c < col_count; ++c) {
                if (!jr[c].is_string())
                    continue;
                std::string cell = jr[c].get<std::string>();
                if (cell.size() > kMaxTextLength)
                    cell.resize(kMaxTextLength);
                row.push_back(std::move(cell));
            }
            t->live_text_rows.push_back(std::move(row));
        }
    }
    if (jt.contains("live_text_column_order") && jt["live_text_column_order"].is_array()) {
        const size_t count = std::min(jt["live_text_column_order"].size(), kMaxLiveTextColumns);
        t->live_text_column_order.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            if (!jt["live_text_column_order"][i].is_string()) continue;
            std::string layer_id = jt["live_text_column_order"][i].get<std::string>();
            if (layer_id.size() > kMaxNameLength)
                layer_id.resize(kMaxNameLength);
            t->live_text_column_order.push_back(std::move(layer_id));
        }
    }
    t->live_text_header_state = bounded_string(jt, "live_text_header_state", "", kMaxTextLength);
    t->external_data_enabled = json_bool(jt, "external_data_enabled", false);
    t->preview_screenshot_png_base64 = bounded_string(jt, "preview_screenshot_png_base64", "",
                                                       kMaxScreenshotBase64Length);

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
        for (auto &layer_id : t->live_text_column_order) {
            auto it = layer_id_map.find(layer_id);
            if (it != layer_id_map.end())
                layer_id = it->second;
        }
    }

    return t;
}

void TitleDataStore::save() const
{
    std::vector<std::shared_ptr<Title>> snapshot;
    std::string path;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        snapshot = titles_;
        path = loaded_path_.empty() ? data_path() : loaded_path_;
    }

    json root = json::array();
    for (auto &t : snapshot) {
        if (t)
            root.push_back(title_to_json(*t));
    }

    const std::string tmp_path = path + ".tmp";
    {
        std::ofstream f(tmp_path, std::ios::binary | std::ios::trunc);
        if (!f.is_open()) {
            blog(LOG_WARNING, "[OBS Graphics Studio Pro] Failed to open titles.json for saving");
            return;
        }
        f << root.dump(2);
        if (!f.good()) {
            blog(LOG_WARNING, "[OBS Graphics Studio Pro] Failed while writing titles.json");
            std::remove(tmp_path.c_str());
            return;
        }
    }

    if (std::rename(tmp_path.c_str(), path.c_str()) != 0) {
        std::remove(path.c_str());
        if (std::rename(tmp_path.c_str(), path.c_str()) != 0) {
            blog(LOG_WARNING, "[OBS Graphics Studio Pro] Failed to replace titles.json");
            std::remove(tmp_path.c_str());
        }
    }
}

bool TitleDataStore::export_title(const std::string &id, const std::string &path, std::string *error) const
{
    TitleTemplateExportMetadata metadata;
    return export_title(id, path, metadata, error);
}

bool TitleDataStore::export_title(const std::string &id, const std::string &path,
                                  const TitleTemplateExportMetadata &metadata,
                                  std::string *error) const
{
    if (error) error->clear();
    auto t = get_title(id);
    if (!t) {
        if (error) *error = "No title template is selected.";
        return false;
    }

    TitleTemplateExportMetadata export_metadata = metadata;
    if (export_metadata.title.empty()) export_metadata.title = t->name;
    if (export_metadata.description.empty()) export_metadata.description = t->description;
    if (export_metadata.creator.empty()) export_metadata.creator = t->creator;
    if (export_metadata.creation_date.empty()) {
        export_metadata.creation_date = t->creation_date.empty() ? current_iso_utc_string() : t->creation_date;
    }
    if (export_metadata.screenshot_png_base64.empty())
        export_metadata.screenshot_png_base64 = t->preview_screenshot_png_base64;

    json root;
    root["format"] = "obs-graphics-studio-pro-title-template";
    root["version"] = 3;
    root["template_title"] = export_metadata.title;
    root["description"] = export_metadata.description;
    root["creator"] = export_metadata.creator;
    root["creation_date"] = export_metadata.creation_date;
    root["screenshot"] = {
        {"mime_type", "image/png"},
        {"data_base64", export_metadata.screenshot_png_base64},
    };
    root["metadata"] = {
        {"title", export_metadata.title},
        {"description", export_metadata.description},
        {"creator", export_metadata.creator},
        {"creation_date", export_metadata.creation_date},
        {"screenshot", root["screenshot"]},
    };
    Title exported_copy = *t;
    exported_copy.name = export_metadata.title;
    exported_copy.description = export_metadata.description;
    exported_copy.creator = export_metadata.creator;
    exported_copy.creation_date = export_metadata.creation_date;
    exported_copy.preview_screenshot_png_base64 = export_metadata.screenshot_png_base64;
    json exported_title = title_to_json(exported_copy, true, true, error);
    if ((error && !error->empty()) || exported_title.empty()) {
        if (error && error->empty())
            *error = "Could not embed all title assets in the export file.";
        return false;
    }
    root["title"] = std::move(exported_title);

    std::ofstream f(path, std::ios::binary | std::ios::trunc);
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
    if (error) error->clear();
    try {
        json root;
        if (!read_json_file(path, root, error))
            return nullptr;
        json jt;
        if (root.is_object() && root.contains("title"))
            jt = root["title"];
        else if (root.is_array() && !root.empty())
            jt = root.front();
        else if (root.is_object())
            jt = root;
        else
            throw std::runtime_error("Unsupported template file format.");

        auto imported = title_from_json(jt, true, true, error);
        if (imported && root.is_object()) {
            json meta = root.value("metadata", json::object());
            if (imported->name.empty())
                imported->name = bounded_string(meta, "title", bounded_string(root, "template_title", "Imported Title", kMaxNameLength), kMaxNameLength);
            if (imported->description.empty())
                imported->description = bounded_string(meta, "description", bounded_string(root, "description", "", kMaxTextLength), kMaxTextLength);
            if (imported->creator.empty())
                imported->creator = bounded_string(meta, "creator", bounded_string(root, "creator", "", kMaxNameLength), kMaxNameLength);
            if (imported->creation_date.empty())
                imported->creation_date = bounded_string(meta, "creation_date", bounded_string(root, "creation_date", "", kMaxNameLength), kMaxNameLength);
        }
        if (imported && imported->preview_screenshot_png_base64.empty() && root.is_object()) {
            json screenshot = root.value("screenshot", json::object());
            if (screenshot.empty() && root.contains("metadata") && root["metadata"].is_object())
                screenshot = root["metadata"].value("screenshot", json::object());
            if (screenshot.is_object()) {
                const std::string png_base64 = bounded_string(screenshot, "data_base64", "",
                                                              kMaxScreenshotBase64Length);
                imported->preview_screenshot_png_base64 = png_base64;
            }
        }
        if (error && !error->empty())
            throw std::runtime_error(*error);
        if (!imported || imported->layers.empty())
            throw std::runtime_error("Template data was empty.");
        std::unordered_set<std::string> seen_ids;
        ensure_unique_title_id(imported, seen_ids);

        std::string base_name = imported->name.empty() ? "Imported Title" : imported->name;
        std::string unique_name = base_name;
        int suffix = 2;
        {
            std::lock_guard<std::recursive_mutex> lock(mutex_);
            auto name_exists = [this](const std::string &candidate) {
                return std::any_of(titles_.begin(), titles_.end(), [&](const auto &existing) {
                    return existing && existing->name == candidate;
                });
            };
            while (name_exists(unique_name))
                unique_name = base_name + " (imported " + std::to_string(suffix++) + ")";
            imported->name = unique_name;
            titles_.push_back(imported);
        }

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
    const std::string path = data_path();
    json root;
    std::string error;
    if (!read_json_file(path, root, &error)) {
        {
            std::lock_guard<std::recursive_mutex> lock(mutex_);
            loaded_path_ = path;
            titles_.clear();
        }
        notify_change();
        if (error == "Could not open the file.")
            blog(LOG_INFO, "[OBS Graphics Studio Pro] No saved titles found for this scene collection, starting fresh.");
        else
            blog(LOG_WARNING, "[OBS Graphics Studio Pro] Failed to read scene collection titles file: %s", error.c_str());
        return;
    }

    try {
        if (!root.is_array())
            throw std::runtime_error("Saved titles root must be an array.");

        std::vector<std::shared_ptr<Title>> loaded;
        std::unordered_set<std::string> seen_ids;
        const size_t count = std::min(root.size(), kMaxTitles);
        loaded.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            auto title = title_from_json(root[i], false);
            ensure_unique_title_id(title, seen_ids);
            loaded.push_back(title);
        }
        size_t loaded_count = 0;
        {
            std::lock_guard<std::recursive_mutex> lock(mutex_);
            loaded_path_ = path;
            titles_ = std::move(loaded);
            loaded_count = titles_.size();
        }
        notify_change();
        blog(LOG_INFO, "[OBS Graphics Studio Pro] Loaded %zu title(s) for this scene collection.", loaded_count);
    } catch (std::exception &e) {
        {
            std::lock_guard<std::recursive_mutex> lock(mutex_);
            loaded_path_ = path;
            titles_.clear();
        }
        notify_change();
        blog(LOG_WARNING, "[OBS Graphics Studio Pro] Failed to parse scene collection titles file: %s", e.what());
    }
}
