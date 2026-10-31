/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QObject>
#include <QString>

namespace TrophyBackend {

/**
 * @brief Desktop integration for portable (AppImage) builds.
 *
 * An AppImage runs from a transient /tmp mount, so the host portal never
 * sees an app id for "trophy" and file dialogs / global shortcuts degrade.
 * This helper copies the AppImage to a stable path, writes a .desktop entry
 * and installs the icon, so the host registers the app id. It is idempotent:
 * if a matching .desktop + stable binary already exist, integrate() does
 * nothing.
 */
class DesktopIntegration : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool isAppImage READ isAppImage CONSTANT)
    Q_PROPERTY(bool isIntegrated READ isIntegrated NOTIFY integrationChanged)
    Q_PROPERTY(QString stablePath READ stablePath CONSTANT)
    Q_PROPERTY(bool declined READ declined WRITE setDeclined NOTIFY declinedChanged)

public:
    explicit DesktopIntegration(QObject *parent = nullptr);

    // True when running from an AppImage bundle.
    bool isAppImage() const;

    // Stable location the AppImage is copied to.
    QString stablePath() const;

    // Path of the .desktop file that registers the app id.
    QString desktopFilePath() const;

    // True when a matching .desktop + stable binary already exist.
    bool isIntegrated() const;

    bool declined() const;
    void setDeclined(bool declined);

    // Perform integration. Idempotent: returns true without work when
    // already integrated. Returns false on failure.
    Q_INVOKABLE bool integrate();

signals:
    void integrationChanged();
    void declinedChanged();
    void integrationFinished(bool ok, const QString &message);

private:
    QString appImagePath() const;
    QString iconTargetPath() const;
    bool refreshCaches(const QString &desktopPath);
};

} // namespace TrophyBackend