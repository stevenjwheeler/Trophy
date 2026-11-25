/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QObject>

class QWheelEvent;

namespace TrophyBackend {

/**
 * Makes mouse-wheel scrolling distance depend on the wheel rotation, not on
 * how much of QQuickFlickable's animation the next event left it.
 *
 * QQuickFlickable animates every wheel event's movement with an OutExpo curve
 * over three quarters of its 400 ms fixup duration and restarts that animation
 * for each event that arrives. A conventional wheel sends one 120-unit event
 * per notch, so the animation has time to finish. A high-resolution wheel
 * (Logitech "Scroll Wheel Resolution" via Solaar, or any HID++ smooth-scroll
 * device) reports the same rotation as several sub-notch events a few
 * milliseconds apart, so each event only delivers the first fraction of its
 * distance and scrolling becomes several times slower than requested.
 *
 * The normalizer applies the exact rotation-derived distance immediately for
 * non-synthesized (mouse wheel) events. Touchpads keep Qt's pixel-delta path,
 * which is already rate independent. It only acts when the Flickable itself is
 * the event receiver, i.e. after every item under the cursor has ignored the
 * wheel: sliders, spinners, combo box popups and the player's zoom handlers
 * keep their own behaviour. When the Flickable cannot move any further the
 * event is left unhandled so a nested scrollable can consume it.
 */
class WheelScrollNormalizer : public QObject {
    Q_OBJECT

public:
    explicit WheelScrollNormalizer(QObject *parent = nullptr);

    /// Install the single application-wide instance. No-op when already
    /// installed, and kept alive for the lifetime of the application.
    static void install();

    /// Move \a receiver by the rotation in \a event when it is a Flickable that
    /// can consume the wheel, returning true when the event was consumed.
    /// Exposed so tests can drive the decision without a window.
    static bool consumeWheel(QObject *receiver, QWheelEvent *event);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
};

} // namespace TrophyBackend
