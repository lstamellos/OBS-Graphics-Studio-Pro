/*
 * OBS Graphics Studio Pro Plugin - plugin-main.cpp
 * Entry point: registers the source, dock, and module lifecycle.
 */

#include "plugin-main.h"
#include "title-source.h"
#include "title-dock.h"
#include "title-hotkeys.h"
#include "title-data.h"
#include "title-localization.h"
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <QMainWindow>
#include <QAction>
#include <QDockWidget>
#include <QMenu>
#include <QMenuBar>
#include <QSignalBlocker>
#include <QString>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

/* ── forward declarations ───────────────────────────────────────── */
static void on_frontend_event(obs_frontend_event event, void *priv);

/* ── module globals ─────────────────────────────────────────────── */
static TitleDock *g_dock = nullptr;
static QAction *g_dock_menu_action = nullptr;
static bool g_frontend_ready = false;


static QMenu *find_docks_menu(QMainWindow *main)
{
    if (!main || !main->menuBar()) return nullptr;
    for (auto *menu : main->menuBar()->findChildren<QMenu *>()) {
        QString title = menu->title();
        title.remove('&');
        if (title.compare(obsgs_tr("OBSTitles.DocksMenu"), Qt::CaseInsensitive) == 0)
            return menu;
    }
    return nullptr;
}

static void add_docks_menu_entry(QMainWindow *main)
{
    QMenu *docks_menu = find_docks_menu(main);
    if (!docks_menu || !g_dock || g_dock_menu_action) return;

    g_dock_menu_action = docks_menu->addAction(obsgs_tr("OBSTitles.DockName"));
    g_dock_menu_action->setObjectName("obs-graphics-studio-pro-docks-menu-action");
    g_dock_menu_action->setCheckable(true);
    g_dock_menu_action->setChecked(g_dock->isVisible());
    QObject::connect(g_dock_menu_action, &QAction::triggered, g_dock,
                     [](bool visible) { if (g_dock) g_dock->setVisible(visible); });
    QObject::connect(g_dock, &QDockWidget::visibilityChanged, g_dock_menu_action,
                     [](bool visible) {
                         if (!g_dock_menu_action) return;
                         QSignalBlocker blocker(g_dock_menu_action);
                         g_dock_menu_action->setChecked(visible);
                     });
}

/* ── module load ────────────────────────────────────────────────── */
bool obs_module_load(void)
{
    blog(LOG_INFO, "[OBS Graphics Studio Pro] Loading plugin v%s", PLUGIN_VERSION);

    /* 1. Initialise persistent title store */
    TitleDataStore::instance().load();

    /* 2. Register the renderable source type and title cue hotkeys */
    title_source_register();
    title_hotkeys_register();

    /* 3. Defer dock and hotkey creation until the OBS UI is ready */
    obs_frontend_add_event_callback(on_frontend_event, nullptr);

    blog(LOG_INFO, "[OBS Graphics Studio Pro] Plugin loaded.");
    return true;
}

/* ── module unload ──────────────────────────────────────────────── */
void obs_module_unload(void)
{
    title_hotkeys_unregister();
    TitleDataStore::instance().save();
    obs_frontend_remove_event_callback(on_frontend_event, nullptr);
    title_hotkeys_unregister();
    if (g_dock_menu_action) {
        delete g_dock_menu_action;
        g_dock_menu_action = nullptr;
    }
    obs_frontend_remove_dock("obs-graphics-studio-pro-dock");
    blog(LOG_INFO, "[OBS Graphics Studio Pro] Plugin unloaded.");
}

/* ── frontend event handler ─────────────────────────────────────── */
static void on_frontend_event(obs_frontend_event event, void * /*priv*/)
{
    if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING) {
        TitleDataStore::instance().load();

        QMainWindow *main =
            static_cast<QMainWindow *>(obs_frontend_get_main_window());

        g_dock = new TitleDock(main);
        g_dock->setObjectName("OBSGraphicsStudioProDock");
        g_dock->setWindowTitle(obsgs_tr("OBSTitles.DockName"));

        obs_frontend_add_custom_qdock("obs-graphics-studio-pro-dock", g_dock);
        add_docks_menu_entry(main);
        g_frontend_ready = true;
        title_hotkeys_register();
        blog(LOG_INFO, "[OBS Graphics Studio Pro] Dock and title cue hotkeys registered.");
    }

    if (event == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP) {
        TitleDataStore::instance().save();
        title_hotkeys_unregister();
    }

    if (event == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED && g_frontend_ready) {
        TitleDataStore::instance().load();
        title_hotkeys_register();
        if (g_dock)
            g_dock->update_scene_collection_title();
    }

    if (event == OBS_FRONTEND_EVENT_EXIT) {
        g_frontend_ready = false;
        title_hotkeys_unregister();
        TitleDataStore::instance().save();
    }
}
