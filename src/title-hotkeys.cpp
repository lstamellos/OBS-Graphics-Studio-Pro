#include "title-hotkeys.h"
#include "title-data.h"
#include <obs-module.h>

#include <algorithm>
#include <cctype>
#include <memory>
#include <sstream>
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

std::vector<HotkeyRegistration> g_hotkeys;
std::string g_hotkey_signature;
bool g_hotkeys_active = false;

static std::vector<std::shared_ptr<Layer>> exposed_text_layers(const std::shared_ptr<Title> &title)
{
    std::vector<std::shared_ptr<Layer>> exposed;
    if (!title) return exposed;
    for (const auto &layer : title->layers) {
        if ((layer->type == LayerType::Text || layer->type == LayerType::Ticker) && layer->expose_text)
            exposed.push_back(layer);
    }
    return exposed;
}

static void normalize_live_text_rows(const std::shared_ptr<Title> &title,
                                     const std::vector<std::shared_ptr<Layer>> &exposed)
{
    if (!title || exposed.empty()) return;
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

static std::string cue_description(const std::shared_ptr<Title> &title, int cue_number)
{
    return title_display_name(title) + " — " + obs_module_text("OBSTitles.Cue") + " " + std::to_string(cue_number);
}

static void cue_title_row(const std::shared_ptr<Title> &title, int row)
{
    if (!title) return;

    auto exposed = exposed_text_layers(title);
    normalize_live_text_rows(title, exposed);

    if (exposed.empty()) {
        title->current_cue_row = -1;
        title->pending_cue_row = -1;
    } else {
        if (row < 0 || row >= (int)title->live_text_rows.size()) return;
        const bool needs_outro_before_cue =
            (title->playback_mode == 1 || title->playback_mode == 2) &&
            title->current_cue_row >= 0 && title->current_cue_row != row;
        if (needs_outro_before_cue) {
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

static std::vector<HotkeyDescriptor> build_descriptors()
{
    std::vector<HotkeyDescriptor> descriptors;

    for (const auto &title : TitleDataStore::instance().titles()) {
        if (!title) continue;

        const std::string safe_title_id = hotkey_safe_id(title->id);
        auto exposed = exposed_text_layers(title);
        normalize_live_text_rows(title, exposed);

        if (exposed.empty()) {
            descriptors.push_back({
                "obs_graphics_studio_pro." + safe_title_id + ".cue.title",
                title_display_name(title) + " — " + obs_module_text("OBSTitles.Cue"),
                title->id,
                HotkeyAction::CueRow,
                -1,
            });
            continue;
        }

        descriptors.push_back({
            "obs_graphics_studio_pro." + safe_title_id + ".cue.next",
            title_display_name(title) + " — " + obs_module_text("OBSTitles.NextCue"),
            title->id,
            HotkeyAction::NextCue,
            -1,
        });
        descriptors.push_back({
            "obs_graphics_studio_pro." + safe_title_id + ".cue.previous",
            title_display_name(title) + " — " + obs_module_text("OBSTitles.PreviousCue"),
            title->id,
            HotkeyAction::PreviousCue,
            -1,
        });

        for (int row = 0; row < (int)title->live_text_rows.size(); ++row) {
            descriptors.push_back({
                "obs_graphics_studio_pro." + safe_title_id + ".cue." + std::to_string(row + 1),
                cue_description(title, row + 1),
                title->id,
                HotkeyAction::CueRow,
                row,
            });
        }
    }

    return descriptors;
}

static std::string descriptor_signature(const std::vector<HotkeyDescriptor> &descriptors)
{
    std::ostringstream out;
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
        if (hotkey.id != OBS_INVALID_HOTKEY_ID)
            obs_hotkey_unregister(hotkey.id);
    }
    g_hotkeys.clear();
    g_hotkey_signature.clear();
}

static void refresh_hotkeys()
{
    if (!g_hotkeys_active) return;

    auto descriptors = build_descriptors();
    std::string signature = descriptor_signature(descriptors);
    if (signature == g_hotkey_signature) return;

    unregister_all_hotkeys();
    g_hotkeys.reserve(descriptors.size());
    for (auto &descriptor : descriptors) {
        g_hotkeys.push_back({OBS_INVALID_HOTKEY_ID, std::move(descriptor)});
        auto &registration = g_hotkeys.back();
        registration.id = obs_hotkey_register_frontend(
            registration.descriptor.name.c_str(),
            registration.descriptor.description.c_str(),
            hotkey_callback,
            &registration.descriptor);
    }
    g_hotkey_signature = std::move(signature);
}

} // namespace

void title_hotkeys_register()
{
    if (g_hotkeys_active) return;
    g_hotkeys_active = true;
    refresh_hotkeys();
    TitleDataStore::instance().on_change([]() { refresh_hotkeys(); });
}

void title_hotkeys_unregister()
{
    g_hotkeys_active = false;
    unregister_all_hotkeys();
}
