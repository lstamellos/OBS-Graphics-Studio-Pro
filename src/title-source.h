/*
 * title-source.h
 *
 * OBS source type "obs_titles_source".
 * Renders a Title (from TitleDataStore) into an OBS texture every
 * frame using Cairo for 2-D compositing.
 */

#pragma once

#include <obs-module.h>
#include <string>

/* Registers the source type with OBS. Call once from obs_module_load(). */
void title_source_register();

/* Source settings keys */
#define PROP_TITLE_ID      "title_id"
#define PROP_LOOP          "loop"
#define PROP_SPEED         "speed"
#define PROP_AUTO_ADVANCE  "auto_advance"
