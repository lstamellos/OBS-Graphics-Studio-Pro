/*
 * OBS Titler Pro Plugin - plugin-main.cpp
 * Entry point: registers the source, dock, and module lifecycle.
 */

#include "plugin-main.h"
#include "title-source.h"
#include "title-dock.h"
#include "title-data.h"
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <QMainWindow>
#include <QAction>
#include <QDockWidget>
#include <QMenu>
#include <QMenuBar>
#include <QString>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

/* ── forward declarations ───────────────────────────────────────── */
static void on_frontend_event(obs_frontend_event event, void *priv);

/* ── module globals ─────────────────────────────────────────────── */
static TitleDock *g_dock = nullptr;
static QAction *g_dock_menu_action = nullptr;


static QMenu *find_docks_menu(QMainWindow *main)
{
    if (!main || !main->menuBar()) return nullptr;
    for (auto *menu : main->menuBar()->findChildren<QMenu *>()) {
        QString title = menu->title();
        title.remove('&');
        if (title.compare("Docks", Qt::CaseInsensitive) == 0)
            return menu;
    }
    return nullptr;
}

static void add_docks_menu_entry(QMainWindow *main)
{
    QMenu *docks_menu = find_docks_menu(main);
    if (!docks_menu || !g_dock || g_dock_menu_action) return;

    g_dock_menu_action = docks_menu->addAction("OBS Titler Pro");
    g_dock_menu_action->setObjectName("obs-titler-pro-docks-menu-action");
    g_dock_menu_action->setCheckable(true);
    g_dock_menu_action->setChecked(g_dock->isVisible());
    QObject::connect(g_dock_menu_action, &QAction::toggled, g_dock,
                     [](bool visible) { if (g_dock) g_dock->setVisible(visible); });
    QObject::connect(g_dock, &QDockWidget::visibilityChanged, g_dock_menu_action,
                     [](bool visible) { if (g_dock_menu_action) g_dock_menu_action->setChecked(visible); });
}

/* ── module load ────────────────────────────────────────────────── */
bool obs_module_load(void)
{
    blog(LOG_INFO, "[OBS Titler Pro] Loading plugin v%s", PLUGIN_VERSION);

    /* 1. Initialise persistent title store */
    TitleDataStore::instance().load();

    /* 2. Register the renderable source type */
    title_source_register();

    /* 3. Defer dock creation until the OBS UI is ready */
    obs_frontend_add_event_callback(on_frontend_event, nullptr);

    blog(LOG_INFO, "[OBS Titler Pro] Plugin loaded.");
    return true;
}

/* ── module unload ──────────────────────────────────────────────── */
void obs_module_unload(void)
{
    TitleDataStore::instance().save();
    obs_frontend_remove_event_callback(on_frontend_event, nullptr);
    if (g_dock_menu_action) {
        delete g_dock_menu_action;
        g_dock_menu_action = nullptr;
    }
    obs_frontend_remove_dock("obs-titler-pro-dock");
    blog(LOG_INFO, "[OBS Titler Pro] Plugin unloaded.");
}

/* ── frontend event handler ─────────────────────────────────────── */
static void on_frontend_event(obs_frontend_event event, void * /*priv*/)
{
    if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING) {
        QMainWindow *main =
            static_cast<QMainWindow *>(obs_frontend_get_main_window());

        g_dock = new TitleDock(main);
        g_dock->setObjectName("OBSTitlerProDock");
        g_dock->setWindowTitle("OBS Titler Pro");

        obs_frontend_add_custom_qdock("obs-titler-pro-dock", g_dock);
        add_docks_menu_entry(main);
        blog(LOG_INFO, "[OBS Titler Pro] Dock registered.");
    }

    if (event == OBS_FRONTEND_EVENT_EXIT) {
        TitleDataStore::instance().save();
    }
}
