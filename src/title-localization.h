#pragma once

#include <obs-module.h>
#include <QString>

static inline QString obsgs_tr(const char *key)
{
    const char *text = obs_module_text(key);
    return QString::fromUtf8((text && *text) ? text : key);
}

static inline const char *obsgs_tr_c(const char *key)
{
    const char *text = obs_module_text(key);
    return (text && *text) ? text : key;
}
