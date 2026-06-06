/*
 * title-source.h
 *
 * OBS source type "obs_graphics_studio_pro_source".
 * Renders a Title (from TitleDataStore) through the OBS-native GPU
 * pipeline.  The live source path avoids CPU 2-D raster backends and
 * uses libobs gs_* passes for drawing, transforms, blending, and effects.
 */

#pragma once

#include <obs-module.h>
#include <string>
#include <QImage>

struct Title;

/* Registers the source type with OBS. Call once from obs_module_load(). */
void title_source_register();
QImage render_title_to_image(const Title &title, double t);

/* Source settings keys */
#define PROP_TITLE_ID      "title_id"
#define PROP_LOOP          "loop"
#define PROP_SPEED         "speed"
#define PROP_AUTO_ADVANCE  "auto_advance"
