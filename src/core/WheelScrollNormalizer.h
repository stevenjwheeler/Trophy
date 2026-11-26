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
 * The normalizer accumulates the exact rotation-derived distance for
 * non-synthesized (mouse wheel) events and glides to it over a short
 * ease-out, so every step is kept without snapping. Touchpads keep Qt's
 * pixel-delta path, which is already rate independent. It only acts when the
 * Flickable itself is the event receiver, i.e. after every item under the
 * cursor has ignored the wheel: sliders, spinners, combo box popups and the
 * player's zoom handlers keep their own behaviour.
 *
 * At the end of the content a nested Flickable leaves the event unhandled so
 * an ancestor scrollable can consume it. The outermost Flickable answers the
 * wheel itself: a clipped view (ScrollView, the library lists, Settings)
 * stretches past the end and springs back elastically once the wheel stops,
 * while an unclipped view (whose content would spill over the page below)
 * recoils a few pixels inward instead. Both consume the event.
 * A view exposing wheelTopStretch renders that signed top overscroll with a
 * content-item Translate instead of moving its native viewport out of bounds.
 * Games uses this to avoid section-estimation fixups during rapid top pulls.
 *
 * Any external content move (a section jump animation, a drag, a layout
 * change) cancels the current animation and the rubber band so the two never
 * fight over contentY.
 */
class WheelScrollNormalizer : public QObject {
    Q_OBJECT

public:
    explicit WheelScrollNormalizer(QObject *parent = nullptr);

    /// Install the single application-wide instance. No-op when already
    /// installed, and kept alive for the lifetime of the application.
    static void install();

    /// Start the scroll described by \a event on \a receiver when it is a
    /// Flickable that can consume the wheel, returning true when the event was
    /// consumed. The movement is animated, so the resting position is reached
    /// shortly after this returns. Exposed so tests can drive the decision
    /// without a window.
    static bool consumeWheel(QObject *receiver, QWheelEvent *event);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
};

} // namespace TrophyBackend
