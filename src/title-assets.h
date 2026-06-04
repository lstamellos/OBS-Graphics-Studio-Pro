#pragma once

#include <obs-module.h>
#include <QIcon>
#include <QString>

static inline QIcon obsgs_icon(const char *file_name)
{
    QString rel = QStringLiteral("icons/") + QString::fromUtf8(file_name);
    char *path = obs_module_file(rel.toUtf8().constData());
    if (!path)
        return QIcon();

    QIcon icon(QString::fromUtf8(path));
    bfree(path);
    return icon;
}
