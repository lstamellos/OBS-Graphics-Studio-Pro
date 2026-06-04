#pragma once

#include <obs-module.h>
#include <QApplication>
#include <QByteArray>
#include <QColor>
#include <QFile>
#include <QIcon>
#include <QIODevice>
#include <QPainter>
#include <QPalette>
#include <QPixmap>
#include <QString>
#include <QSvgRenderer>

static inline QColor obsgs_icon_color()
{
    const QPalette palette = qApp ? qApp->palette() : QPalette();
    const QColor window = palette.color(QPalette::Window);
    return window.lightness() < 128 ? QColor(0xf2, 0xf2, 0xf2) : QColor(0x20, 0x20, 0x20);
}

static inline QIcon obsgs_icon(const char *file_name)
{
    QString rel = QStringLiteral("icons/") + QString::fromUtf8(file_name);
    char *path = obs_module_file(rel.toUtf8().constData());
    if (!path)
        return QIcon();

    const QString icon_path = QString::fromUtf8(path);
    bfree(path);

    QFile file(icon_path);
    if (!file.open(QIODevice::ReadOnly))
        return QIcon(icon_path);

    QByteArray svg = file.readAll();
    svg.replace("currentColor", obsgs_icon_color().name(QColor::HexRgb).toUtf8());

    QSvgRenderer renderer(svg);
    if (!renderer.isValid())
        return QIcon(icon_path);

    QIcon icon;
    const int sizes[] = {16, 20, 24, 32};
    for (int size : sizes) {
        QPixmap pixmap(size, size);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        renderer.render(&painter);
        icon.addPixmap(pixmap);
    }
    return icon;
}
