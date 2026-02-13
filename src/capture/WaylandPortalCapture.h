/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2024
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "IVideoCaptureSource.h"
#include <atomic>

namespace GameRecorder {

/**
 * @brief Wayland screen capture using XDG Desktop Portal
 *
 * Uses the portal API to request screen sharing permission and
 * PipeWire to receive the video stream.
 */
class WaylandPortalCapture : public IVideoCaptureSource
{
    Q_OBJECT

public:
    explicit WaylandPortalCapture(QObject *parent = nullptr);
    ~WaylandPortalCapture() override;

    bool initialize(const CaptureConfig &config) override;
    void setFrameCallback(FrameCallback callback) override;
    bool startCapture() override;
    void stopCapture() override;
    bool isCapturing() const override;
    CaptureCapabilities capabilities() const override;
    QSize nativeResolution() const override;
    QString name() const override { return "Wayland Portal Capture"; }

    Q_INVOKABLE void requestScreenShare();

signals:
    void screenShareGranted(const QString &pipewireNodeId);
    void screenShareDenied();

private:
    void setupPortalConnection();

    FrameCallback m_callback;
    CaptureConfig m_config;
    std::atomic<bool> m_capturing{false};
    QSize m_resolution;
    int m_pipewireNodeId = -1;
};

} // namespace GameRecorder
