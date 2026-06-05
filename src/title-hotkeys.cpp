#include "title-hotkeys.h"
#include "title-data.h"
#include <obs-module.h>
#include <QSettings>
#include <QString>

#include <algorithm>
#include <cctype>
#include <map>
#include <memory>
#include <sstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

enum class HotkeyAction {
    CueRow,
    NextCue,
    PreviousCue,
};

struct HotkeyDescriptor {
    std::string name;
    std::string description;
    std::string title_id;
    HotkeyAction action = HotkeyAction::CueRow;
    int row = -1;
};

struct HotkeyRegistration {
    obs_hotkey_id id = OBS_INVALID_HOTKEY_ID;
    HotkeyDescriptor descriptor;
};

struct HotkeySection {
    std::string title_id;
    std::string display_name;
    /* Lightweight source used only as the OBS Hotkeys settings section. */
    obs_source_t *source = nullptr;
};

std::vector<HotkeySection> g_sections;
std::vector<HotkeyRegistration> g_hotkeys;
std::map<std::string, obs_source_t *> g_section_sources;
std::string g_hotkey_signature;
std::map<std::string, std::string> g_persisted_hotkey_bindings;
bool g_hotkeys_active = false;
bool g_change_callback_registered = false;
bool g_hotkey_section_source_registered = false;

constexpr const char *kHotkeySectionSourceId = "obs_graphics_studio_pro_hotkey_section";
constexpr const char *kDockSettingsGroup = "TitleDock";
constexpr const char *kBackgroundPersistenceKey = "backgroundPersistence";
constexpr const char *kTextPersistenceKey = "textPersistence";
constexpr const char *kHotkeySettingsGroup = "Hotkeys";

static void load_persisted_hotkey_bindings()
{
    QSettings settings(QStringLiteral("OBSGraphicsStudioPro"), QStringLiteral("Dock"));
    settings.beginGroup(QString::fromUtf8(kHotkeySettingsGroup));
    g_persisted_hotkey_bindings.clear();
    for (const auto &key : settings.childKeys())
        g_persisted_hotkey_bindings[key.toStdString()] = settings.value(key).toString().toStdString();
    settings.endGroup();
}

static void save_persisted_hotkey_bindings()
{
    QSettings settings(QStringLiteral("OBSGraphicsStudioPro"), QStringLiteral("Dock"));
    settings.beginGroup(QString::fromUtf8(kHotkeySettingsGroup));
    settings.remove(QString());
    for (const auto &[name, json] : g_persisted_hotkey_bindings)
        settings.setValue(QString::fromStdString(name), QString::fromStdString(json));
    settings.endGroup();
}

static void remember_hotkey_binding(const HotkeyRegistration &hotkey)
{
    if (hotkey.id == OBS_INVALID_HOTKEY_ID) return;
    obs_data_array_t *bindings = obs_hotkey_save(hotkey.id);
    if (!bindings) return;

    obs_data_t *wrapper = obs_data_create();
    obs_data_set_array(wrapper, "bindings", bindings);
    const char *json = obs_data_get_json(wrapper);
    if (json && *json)
        g_persisted_hotkey_bindings[hotkey.descriptor.name] = json;

    obs_data_release(wrapper);
    obs_data_array_release(bindings);
}

static void restore_hotkey_binding(const HotkeyRegistration &hotkey)
{
    if (hotkey.id == OBS_INVALID_HOTKEY_ID) return;
    auto it = g_persisted_hotkey_bindings.find(hotkey.descriptor.name);
    if (it == g_persisted_hotkey_bindings.end() || it->second.empty()) return;

    obs_data_t *wrapper = obs_data_create_from_json(it->second.c_str());
    if (!wrapper) return;
    obs_data_array_t *bindings = obs_data_get_array(wrapper, "bindings");
    if (bindings) {
        obs_hotkey_load(hotkey.id, bindings);
        obs_data_array_release(bindings);
    }
    obs_data_release(wrapper);
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

static void normalize_live_text_rows(const std::shared_ptr<Title> &title,
                                     const std::vector<std::shared_ptr<Layer>> &exposed)
{
    if (!title || exposed.empty()) return;

    std::vector<std::string> new_order;
    new_order.reserve(exposed.size());
    for (const auto &layer : exposed)
        new_order.push_back(layer ? layer->id : std::string());

    const std::vector<std::string> old_order = title->live_text_column_order;
    if (!old_order.empty() && old_order != new_order) {
        for (auto &row : title->live_text_rows) {
            std::vector<std::string> remapped;
            remapped.reserve(exposed.size());
            for (size_t new_col = 0; new_col < new_order.size(); ++new_col) {
                auto it = std::find(old_order.begin(), old_order.end(), new_order[new_col]);
                if (it != old_order.end()) {
                    const size_t old_col = (size_t)std::distance(old_order.begin(), it);
                    remapped.push_back(old_col < row.size() ? row[old_col] : exposed[new_col]->text_content);
                } else {
                    remapped.push_back(exposed[new_col]->text_content);
                }
            }
            row = std::move(remapped);
        }
    }
    title->live_text_column_order = std::move(new_order);

    if (title->live_text_rows.empty()) {
        std::vector<std::string> row;
        for (const auto &layer : exposed)
            row.push_back(layer->text_content);
        title->live_text_rows.push_back(std::move(row));
    }
    for (auto &row : title->live_text_rows) {
        size_t old_size = row.size();
        row.resize(exposed.size());
        for (size_t i = old_size; i < exposed.size(); ++i)
            row[i] = exposed[i]->text_content;
    }
}


static void apply_live_text_row(const std::shared_ptr<Title> &title, int row,
                                const std::vector<std::shared_ptr<Layer>> &exposed)
{
    if (!title || row < 0 || row >= (int)title->live_text_rows.size()) return;
    for (int col = 0; col < (int)exposed.size() && col < (int)title->live_text_rows[row].size(); ++col)
        exposed[col]->text_content = title->live_text_rows[row][col];
}

static std::string hotkey_safe_id(const std::string &value)
{
    std::string out;
    out.reserve(value.size());
    for (unsigned char ch : value) {
        if (std::isalnum(ch))
            out.push_back((char)std::tolower(ch));
        else
            out.push_back('_');
    }
    return out.empty() ? std::string("untitled") : out;
}

static std::string title_display_name(const std::shared_ptr<Title> &title)
{
    return title && !title->name.empty() ? title->name : std::string("Untitled");
}

static std::string program_display_name()
{
    const char *name = obs_module_text("OBSTitles.DockName");
    return name && *name ? std::string(name) : std::string("OBS Graphics Studio Pro");
}

static std::string title_section_name(const std::shared_ptr<Title> &title,
                                      const std::map<std::string, int> &name_counts)
{
    std::string name = title_display_name(title);
    auto it = name_counts.find(name);
    if (it != name_counts.end() && it->second > 1) {
        std::string suffix = title && !title->id.empty()
            ? title->id.substr(0, std::min<size_t>(8, title->id.size()))
            : hotkey_safe_id(name).substr(0, 8);
        name += " [" + suffix + "]";
    }

    return program_display_name() + " - " + name;
}

static std::string cue_description(int cue_number)
{
    return std::string(obs_module_text("OBSTitles.Cue")) + " " + std::to_string(cue_number);
}

static const char *hotkey_section_source_get_name(void *)
{
    return obs_module_text("OBSTitles.DockName");
}

static void register_hotkey_section_source_type()
{
    if (g_hotkey_section_source_registered) return;

    static obs_source_info si = {};
    si.id = kHotkeySectionSourceId;
    si.type = OBS_SOURCE_TYPE_INPUT;
    si.output_flags = 0;
    si.get_name = hotkey_section_source_get_name;

    obs_register_source(&si);
    g_hotkey_section_source_registered = true;
}

static void load_persistence_settings(bool &background_persistence, bool &text_persistence)
{
    QSettings settings(QStringLiteral("OBSGraphicsStudioPro"), QStringLiteral("Dock"));
    settings.beginGroup(QString::fromUtf8(kDockSettingsGroup));
    background_persistence = settings.value(QString::fromUtf8(kBackgroundPersistenceKey), false).toBool();
    text_persistence = background_persistence &&
        settings.value(QString::fromUtf8(kTextPersistenceKey), false).toBool();
    settings.endGroup();
}

static void apply_persistence_settings_to_title(const std::shared_ptr<Title> &title,
                                                const std::vector<std::shared_ptr<Layer>> &exposed)
{
    if (!title) return;
    bool background_persistence = false;
    bool text_persistence = false;
    load_persistence_settings(background_persistence, text_persistence);
    const bool has_exposed = !exposed.empty();
    title->cue_background_persistence = background_persistence && has_exposed;
    title->cue_text_persistence = title->cue_background_persistence && text_persistence;
    if (!title->cue_background_persistence)
        title->cue_persistence_transition = false;
    if (!title->cue_text_persistence)
        title->cue_persistent_text_columns.clear();
}

static void cue_title_row(const std::shared_ptr<Title> &title, int row)
{
    if (!title) return;

    auto exposed = exposed_text_layers(title);
    normalize_live_text_rows(title, exposed);

    if (exposed.empty()) {
        title->current_cue_row = -1;
        title->pending_cue_row = -1;
        title->cue_persistence_transition = false;
        title->cue_persistent_text_columns.clear();
    } else {
        if (row < 0 || row >= (int)title->live_text_rows.size()) return;
        apply_persistence_settings_to_title(title, exposed);
        const bool is_active_cue = title->current_cue_row == row;
        const bool is_pending_cue = title->pending_cue_row == row;
        const int previous_row = title->current_cue_row >= 0 ? title->current_cue_row : title->pending_cue_row;
        const bool can_persist_transition = title->cue_background_persistence &&
            (title->playback_mode == 1 || title->playback_mode == 2) &&
            previous_row >= 0 && previous_row != row;
        const bool needs_outro_before_cue =
            (title->playback_mode == 1 || title->playback_mode == 2) &&
            title->current_cue_row >= 0 && title->current_cue_row != row;

        title->cue_persistence_transition = false;
        title->cue_persistent_text_columns.assign(exposed.size(), false);

        if (is_active_cue || is_pending_cue) {
            title->current_cue_row = -1;
            title->pending_cue_row = -1;
            title->cue_persistence_transition = false;
            title->cue_persistent_text_columns.clear();
        } else if (can_persist_transition) {
            for (int col = 0; col < (int)exposed.size() && col < (int)title->live_text_rows[row].size(); ++col) {
                if (title->cue_text_persistence &&
                    previous_row >= 0 && previous_row < (int)title->live_text_rows.size() &&
                    col < (int)title->live_text_rows[previous_row].size() &&
                    title->live_text_rows[previous_row][col] == title->live_text_rows[row][col])
                    title->cue_persistent_text_columns[col] = true;
            }
            title->pending_cue_row = row;
            title->cue_persistence_transition = true;
        } else if (needs_outro_before_cue) {
            title->pending_cue_row = row;
        } else {
            apply_live_text_row(title, row, exposed);
            title->current_cue_row = row;
            title->pending_cue_row = -1;
        }
    }

    ++title->cue_revision;
    TitleDataStore::instance().save();
    TitleDataStore::instance().notify_change();
}

static void cue_relative(const std::shared_ptr<Title> &title, int delta)
{
    if (!title || delta == 0) return;

    auto exposed = exposed_text_layers(title);
    if (exposed.empty()) return;
    normalize_live_text_rows(title, exposed);
    const int row_count = (int)title->live_text_rows.size();
    if (row_count <= 0) return;

    int base = title->pending_cue_row >= 0 ? title->pending_cue_row : title->current_cue_row;
    int row = 0;
    if (base >= 0 && base < row_count)
        row = (base + delta + row_count) % row_count;
    else if (delta < 0)
        row = row_count - 1;

    cue_title_row(title, row);
}

static void hotkey_callback(void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
    if (!pressed || !data) return;

    const auto *descriptor = static_cast<const HotkeyDescriptor *>(data);
    auto title = TitleDataStore::instance().get_title(descriptor->title_id);
    if (!title) return;

    switch (descriptor->action) {
    case HotkeyAction::CueRow:
        cue_title_row(title, descriptor->row);
        break;
    case HotkeyAction::NextCue:
        cue_relative(title, 1);
        break;
    case HotkeyAction::PreviousCue:
        cue_relative(title, -1);
        break;
    }
}

static std::vector<HotkeyDescriptor> build_descriptors(std::vector<HotkeySection> &sections)
{
    std::vector<HotkeyDescriptor> descriptors;
    std::map<std::string, int> name_counts;

    for (const auto &title : TitleDataStore::instance().titles()) {
        if (title)
            ++name_counts[title_display_name(title)];
    }

    for (const auto &title : TitleDataStore::instance().titles()) {
        if (!title) continue;

        const std::string safe_title_id = hotkey_safe_id(title->id);
        const std::string section_name = title_section_name(title, name_counts);
        sections.push_back({title->id, section_name, nullptr});

        auto exposed = exposed_text_layers(title);
        normalize_live_text_rows(title, exposed);

        if (exposed.empty()) {
            descriptors.push_back({
                "obs_graphics_studio_pro." + safe_title_id + ".cue.title",
                obs_module_text("OBSTitles.Cue"),
                title->id,
                HotkeyAction::CueRow,
                -1,
            });
            continue;
        }

        descriptors.push_back({
            "obs_graphics_studio_pro." + safe_title_id + ".cue.next",
            obs_module_text("OBSTitles.NextCue"),
            title->id,
            HotkeyAction::NextCue,
            -1,
        });
        descriptors.push_back({
            "obs_graphics_studio_pro." + safe_title_id + ".cue.previous",
            obs_module_text("OBSTitles.PreviousCue"),
            title->id,
            HotkeyAction::PreviousCue,
            -1,
        });

        for (int row = 0; row < (int)title->live_text_rows.size(); ++row) {
            descriptors.push_back({
                "obs_graphics_studio_pro." + safe_title_id + ".cue." + std::to_string(row + 1),
                cue_description(row + 1),
                title->id,
                HotkeyAction::CueRow,
                row,
            });
        }
    }

    return descriptors;
}

static std::string descriptor_signature(const std::vector<HotkeyDescriptor> &descriptors,
                                        const std::vector<HotkeySection> &sections)
{
    std::ostringstream out;
    for (const auto &section : sections) {
        out << "section" << '\t'
            << section.title_id << '\t'
            << section.display_name << '\n';
    }
    for (const auto &descriptor : descriptors) {
        out << descriptor.name << '\t'
            << descriptor.description << '\t'
            << descriptor.title_id << '\t'
            << (int)descriptor.action << '\t'
            << descriptor.row << '\n';
    }
    return out.str();
}

static void unregister_all_hotkeys()
{
    for (auto &hotkey : g_hotkeys) {
        if (hotkey.id != OBS_INVALID_HOTKEY_ID) {
            remember_hotkey_binding(hotkey);
            obs_hotkey_unregister(hotkey.id);
        }
    }
    save_persisted_hotkey_bindings();
    g_hotkeys.clear();
    g_hotkey_signature.clear();
}

static void release_hotkey_section_sources()
{
    for (auto &[title_id, source] : g_section_sources) {
        (void)title_id;
        if (source) {
            obs_source_remove(source);
            obs_source_release(source);
        }
    }
    g_section_sources.clear();
    g_sections.clear();
}

static obs_source_t *hotkey_section_source_for(const HotkeySection &section)
{
    auto existing = g_section_sources.find(section.title_id);
    if (existing != g_section_sources.end())
        return existing->second;

    obs_source_t *source = obs_source_create(kHotkeySectionSourceId,
                                             section.display_name.c_str(),
                                             nullptr,
                                             nullptr);
    if (source) {
        obs_source_set_hidden(source, true);
        g_section_sources[section.title_id] = source;
    }
    return source;
}

static void refresh_hotkeys()
{
    if (!g_hotkeys_active) return;

    std::vector<HotkeySection> sections;
    auto descriptors = build_descriptors(sections);
    std::string signature = descriptor_signature(descriptors, sections);
    if (signature == g_hotkey_signature) return;

    unregister_all_hotkeys();
    g_sections = std::move(sections);

    std::map<std::string, obs_source_t *> section_sources;
    for (auto &section : g_sections) {
        section.source = hotkey_section_source_for(section);
        if (section.source)
            section_sources[section.title_id] = section.source;
    }

    g_hotkeys.reserve(descriptors.size());
    for (auto &descriptor : descriptors) {
        auto source_it = section_sources.find(descriptor.title_id);
        if (source_it == section_sources.end() || !source_it->second)
            continue;

        g_hotkeys.push_back({OBS_INVALID_HOTKEY_ID, std::move(descriptor)});
        auto &registration = g_hotkeys.back();
        registration.id = obs_hotkey_register_source(
            source_it->second,
            registration.descriptor.name.c_str(),
            registration.descriptor.description.c_str(),
            hotkey_callback,
            &registration.descriptor);
        restore_hotkey_binding(registration);
    }
    g_hotkey_signature = std::move(signature);
}

} // namespace

void title_hotkeys_register()
{
    if (g_hotkeys_active) return;
    register_hotkey_section_source_type();
    load_persisted_hotkey_bindings();
    g_hotkeys_active = true;
    refresh_hotkeys();
    if (!g_change_callback_registered) {
        TitleDataStore::instance().on_change([]() { refresh_hotkeys(); });
        g_change_callback_registered = true;
    }
}

void title_hotkeys_unregister()
{
    g_hotkeys_active = false;
    unregister_all_hotkeys();
    release_hotkey_section_sources();
}
